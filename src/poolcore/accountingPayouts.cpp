#include "poolcore/accountingPayouts.h"
#include "poolcommon/utils.h"
#include "loguru.hpp"

static const UInt<384> ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE = fromRational(10000u);

CPayoutProcessor::CPayoutProcessor(asyncBase *base,
                                   const PoolBackendConfig &cfg,
                                   const CCoinInfo &coinInfo,
                                   CNetworkClientDispatcher &clientDispatcher,
                                   kvdb<rocksdbBase> &payoutDb,
                                   kvdb<rocksdbBase> &balanceDb,
                                   kvdb<rocksdbBase> &poolBalanceDb,
                                   std::list<PayoutDbRecord> &payoutQueue,
                                   std::unordered_set<std::string> &knownTransactions,
                                   std::map<std::string, UserBalanceRecord> &balanceMap,
                                   const std::unordered_map<std::string, UserSettingsRecord> &userSettings,
                                   const std::atomic<CBackendSettings> &backendSettings) :
  Base_(base),
  Cfg_(cfg),
  CoinInfo_(coinInfo),
  ClientDispatcher_(clientDispatcher),
  PayoutDb_(payoutDb),
  BalanceDb_(balanceDb),
  PoolBalanceDb_(poolBalanceDb),
  PayoutQueue_(payoutQueue),
  KnownTransactions_(knownTransactions),
  BalanceMap_(balanceMap),
  UserSettings_(userSettings),
  BackendSettings_(backendSettings)
{
}

void CPayoutProcessor::buildTransaction(PayoutDbRecord &payout, unsigned index, std::string &recipient, bool *needSkipPayout)
{
  *needSkipPayout = false;
  auto minimalPayout = BackendSettings_.load(std::memory_order_relaxed).PayoutConfig.InstantMinimalPayout;
  if (payout.Value < minimalPayout) {
    LOG_F(INFO,
          "[%u] Accounting: ignore this payout to %s, value is %s, minimal is %s",
          index,
          payout.UserId.c_str(),
          FormatMoney(payout.Value, CoinInfo_.FractionalPartSize).c_str(),
          FormatMoney(minimalPayout, CoinInfo_.FractionalPartSize).c_str());
    *needSkipPayout = true;
    return;
  }

  // Get address for payment
  auto settingsIt = UserSettings_.find(payout.UserId);
  if (settingsIt == UserSettings_.end() || settingsIt->second.Payout.Address.empty()) {
    LOG_F(WARNING, "user %s did not setup payout address, ignoring", payout.UserId.c_str());
    *needSkipPayout = true;
    return;
  }

  const UserSettingsRecord &settings = settingsIt->second;
  recipient = settings.Payout.Address;
  if (!CoinInfo_.checkAddress(settings.Payout.Address, CoinInfo_.PayoutAddressType)) {
    LOG_F(ERROR, "Invalid payment address %s for %s", settings.Payout.Address.c_str(), payout.UserId.c_str());
    *needSkipPayout = true;
    return;
  }

  // Build transaction
  // For bitcoin-based API it's sequential call of createrawtransaction, fundrawtransaction and signrawtransaction
  CNetworkClient::BuildTransactionResult transaction;
  CNetworkClient::EOperationStatus status =
    ClientDispatcher_.ioBuildTransaction(Base_, settings.Payout.Address.c_str(), Cfg_.MiningAddresses.get().MiningAddress, payout.Value, transaction);
  if (status == CNetworkClient::EStatusOk) {
    // Nothing to do
  } else if (status == CNetworkClient::EStatusInsufficientFunds) {
    LOG_F(INFO, "No money left to pay");
    return;
  } else {
    LOG_F(ERROR, "Payment %s to %s failed with error \"%s\"", FormatMoney(payout.Value, CoinInfo_.FractionalPartSize).c_str(), settings.Payout.Address.c_str(), transaction.Error.c_str());
    return;
  }

  // int64_t delta = payout.Value - (transaction.Value + transaction.Fee);
  UInt<384> transactionTotalValue = transaction.Value + transaction.Fee;

  if (payout.Value > transactionTotalValue) {
    // Correct payout value and request balance
    UInt<384> delta = payout.Value - transactionTotalValue;
    payout.Value -= delta;

    // Update user balance
    auto It = BalanceMap_.find(payout.UserId);
    if (It == BalanceMap_.end()) {
      LOG_F(ERROR, "payout to unknown address %s", payout.UserId.c_str());
      return;
    }

    LOG_F(INFO, "   * correct requested balance for %s by %s", payout.UserId.c_str(), FormatMoney(delta, CoinInfo_.FractionalPartSize).c_str());
    UserBalanceRecord &balance = It->second;
    balance.Requested -= delta;
    BalanceDb_.put(balance);
  } else if (payout.Value < transactionTotalValue) {
    LOG_F(ERROR, "Payment %s to %s failed: too big transaction amount", FormatMoney(payout.Value, CoinInfo_.FractionalPartSize).c_str(), settings.Payout.Address.c_str());
    return;
  }

  // Save transaction to database
  if (!KnownTransactions_.insert(transaction.TxId).second) {
    LOG_F(ERROR, "Node generated duplicate for transaction %s !!!", transaction.TxId.c_str());
    return;
  }

  payout.TransactionData = transaction.TxData;
  payout.TransactionId = transaction.TxId;
  payout.Time = time(nullptr);
  payout.Status = PayoutDbRecord::ETxCreated;
  PayoutDb_.put(payout);
}

bool CPayoutProcessor::sendTransaction(PayoutDbRecord &payout)
{
  // Send transaction and change it status to 'Sent'
  // For bitcoin-based API it's 'sendrawtransaction'
  std::string error;
  CNetworkClient::EOperationStatus status = ClientDispatcher_.ioSendTransaction(Base_, payout.TransactionData, payout.TransactionId, error);
  if (status == CNetworkClient::EStatusOk) {
    // Nothing to do
  } else if (status == CNetworkClient::EStatusVerifyRejected) {
    // Sending failed, transaction is rejected
    LOG_F(ERROR, "Transaction %s to %s marked as rejected, removing from database...", payout.TransactionId.c_str(), payout.UserId.c_str());

    // Update transaction in database
    payout.Status = PayoutDbRecord::ETxRejected;
    PayoutDb_.put(payout);

    // Clear all data and re-schedule payout
    payout.TransactionId.clear();
    payout.TransactionData.clear();
    payout.Status = PayoutDbRecord::EInitialized;
    return false;
  } else {
    LOG_F(WARNING, "Sending transaction %s to %s error \"%s\", will try send later...", payout.TransactionId.c_str(), payout.UserId.c_str(), error.c_str());
    return false;
  }

  payout.Status = PayoutDbRecord::ETxSent;
  PayoutDb_.put(payout);
  return true;
}

bool CPayoutProcessor::checkTxConfirmations(PayoutDbRecord &payout)
{
  int64_t confirmations = 0;
  std::string error;
  CNetworkClient::EOperationStatus status = ClientDispatcher_.ioGetTxConfirmations(Base_, payout.TransactionId, &confirmations, &payout.TxFee, error);
  if (status == CNetworkClient::EStatusOk) {
    // Nothing to do
  } else if (status == CNetworkClient::EStatusInvalidAddressOrKey) {
    // Wallet don't know about this transaction
    payout.Status = PayoutDbRecord::ETxCreated;
  } else if (status == CNetworkClient::EStatusVerifyRejected) {
    // Sending failed, transaction is rejected
    LOG_F(ERROR, "Transaction %s to %s marked as rejected, removing from database...", payout.TransactionId.c_str(), payout.UserId.c_str());

    // Update transaction in database
    payout.Status = PayoutDbRecord::ETxRejected;
    PayoutDb_.put(payout);

    // Clear all data and re-schedule payout
    payout.TransactionId.clear();
    payout.TransactionData.clear();
    payout.Status = PayoutDbRecord::EInitialized;
    return false;
  } else {
    LOG_F(WARNING, "Checking transaction %s to %s error \"%s\", will do it later...", payout.TransactionId.c_str(), payout.UserId.c_str(), error.c_str());
    return false;
  }

  // Update database
  if (confirmations >= Cfg_.RequiredConfirmations) {
    payout.Status = PayoutDbRecord::ETxConfirmed;
    PayoutDb_.put(payout);

    // Update user balance
    auto It = BalanceMap_.find(payout.UserId);
    if (It == BalanceMap_.end()) {
      LOG_F(ERROR, "payout to unknown address %s", payout.UserId.c_str());
      return false;
    }

    UserBalanceRecord &balance = It->second;
    // Balance can become negative (unsigned underflow) if fees exceed expectations.
    // This is handled: requestPayout/manualPayoutImpl check isNegative() before queuing new payouts.
    balance.Balance -= (payout.Value + payout.TxFee);
    balance.Requested -= payout.Value;
    balance.Paid += payout.Value;
    BalanceDb_.put(balance);
    return true;
  }

  return false;
}

void CPayoutProcessor::makePayout()
{
  if (!PayoutQueue_.empty()) {
    LOG_F(INFO, "Accounting: checking %u payout requests...", (unsigned)PayoutQueue_.size());

    // Merge small payouts and payouts to invalid address
    // TODO: merge small payouts with normal also
    {
      std::map<std::string, UInt<384>> payoutAccMap;
      for (auto I = PayoutQueue_.begin(), IE = PayoutQueue_.end(); I != IE;) {
        if (I->Status != PayoutDbRecord::EInitialized) {
          ++I;
          continue;
        }

        if (I->Value < BackendSettings_.load(std::memory_order_relaxed).PayoutConfig.InstantMinimalPayout) {
          payoutAccMap[I->UserId] += I->Value;
          LOG_F(INFO,
                "Accounting: merge payout %s for %s (total already %s)",
                FormatMoney(I->Value, CoinInfo_.FractionalPartSize).c_str(),
                I->UserId.c_str(),
                FormatMoney(payoutAccMap[I->UserId], CoinInfo_.FractionalPartSize).c_str());
          PayoutQueue_.erase(I++);
        } else {
          ++I;
        }
      }

      for (const auto &I: payoutAccMap)
        PayoutQueue_.push_back(PayoutDbRecord(I.first, I.second));
    }

    unsigned index = 0;
    for (auto &payout: PayoutQueue_) {
      if (payout.Status == PayoutDbRecord::EInitialized) {
        // Build transaction
        // For bitcoin-based API it's sequential call of createrawtransaction, fundrawtransaction and signrawtransaction
        bool needSkipPayout;
        std::string recipientAddress;
        buildTransaction(payout, index, recipientAddress, &needSkipPayout);
        if (needSkipPayout)
          continue;

        if (payout.Status == PayoutDbRecord::ETxCreated) {
          // Send transaction and change it status to 'Sent'
          // For bitcoin-based API it's 'sendrawtransaction'
          if (sendTransaction(payout))
            LOG_F(INFO, " * sent %s to %s(%s) with txid %s", FormatMoney(payout.Value, CoinInfo_.FractionalPartSize).c_str(), payout.UserId.c_str(), recipientAddress.c_str(), payout.TransactionId.c_str());
        } else {
          // buildTransaction failed — stop processing the queue;
          // this payout will block all subsequent payouts until resolved
          break;
        }
      } else if (payout.Status == PayoutDbRecord::ETxCreated) {
        // Resend transaction
        if (sendTransaction(payout))
          LOG_F(INFO, " * retry send txid %s to %s", payout.TransactionId.c_str(), payout.UserId.c_str());
      } else if (payout.Status == PayoutDbRecord::ETxSent) {
        // Check confirmations
        if (checkTxConfirmations(payout))
          LOG_F(INFO, " * transaction txid %s to %s confirmed", payout.TransactionId.c_str(), payout.UserId.c_str());
      } else {
        // Invalid status
      }
    }

    // Cleanup confirmed payouts
    for (auto I = PayoutQueue_.begin(), IE = PayoutQueue_.end(); I != IE;) {
      if (I->Status == PayoutDbRecord::ETxConfirmed) {
        KnownTransactions_.erase(I->TransactionId);
        PayoutQueue_.erase(I++);
      } else {
        ++I;
      }
    }
  }

  if (!Cfg_.poolZAddr.empty() && !Cfg_.poolTAddr.empty()) {
    // move all to Z-Addr
    CNetworkClient::ListUnspentResult unspent;
    if (ClientDispatcher_.ioListUnspent(Base_, unspent) == CNetworkClient::EStatusOk && !unspent.Outs.empty()) {
      std::unordered_map<std::string, UInt<384>> coinbaseFunds;
      for (const auto &out: unspent.Outs) {
        if (out.IsCoinbase)
          coinbaseFunds[out.Address] += out.Amount;
      }

      for (const auto &out: coinbaseFunds) {
        if (out.second < ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE)
          continue;

        CNetworkClient::ZSendMoneyResult zsendResult;
        CNetworkClient::EOperationStatus status = ClientDispatcher_.ioZSendMoney(Base_, out.first, Cfg_.poolZAddr, out.second, "", 1, UInt<384>::zero(), zsendResult);
        if (status == CNetworkClient::EStatusOk && !zsendResult.AsyncOperationId.empty()) {
          LOG_F(INFO,
                " * moving %s coins from %s to %s started (%s)",
                FormatMoney(out.second, CoinInfo_.FractionalPartSize).c_str(),
                out.first.c_str(),
                Cfg_.poolZAddr.c_str(),
                zsendResult.AsyncOperationId.c_str());
        } else {
          LOG_F(INFO,
                " * async operation start error %s: source=%s, destination=%s, amount=%s",
                !zsendResult.Error.empty() ? zsendResult.Error.c_str() : "<unknown error>",
                out.first.c_str(),
                Cfg_.poolZAddr.c_str(),
                FormatMoney(out.second, CoinInfo_.FractionalPartSize).c_str());
        }
      }
    }

    // move Z-Addr to T-Addr
    UInt<384> zbalance;
    if (ClientDispatcher_.ioZGetBalance(Base_, Cfg_.poolZAddr, &zbalance) == CNetworkClient::EStatusOk && zbalance.nonZero()) {
      LOG_F(INFO, "Accounting: move %s coins to transparent address", FormatMoney(zbalance, CoinInfo_.FractionalPartSize).c_str());
      CNetworkClient::ZSendMoneyResult zsendResult;
      if (ClientDispatcher_.ioZSendMoney(Base_, Cfg_.poolZAddr, Cfg_.poolTAddr, zbalance, "", 1, UInt<384>::zero(), zsendResult) == CNetworkClient::EStatusOk) {
        LOG_F(INFO,
              "moving %s coins from %s to %s started (%s)",
              FormatMoney(zbalance, CoinInfo_.FractionalPartSize).c_str(),
              Cfg_.poolZAddr.c_str(),
              Cfg_.poolTAddr.c_str(),
              !zsendResult.AsyncOperationId.empty() ? zsendResult.AsyncOperationId.c_str() : "<none>");
      }
    }
  }

  // Check consistency
  std::unordered_map<std::string, UInt<384>> enqueued;
  for (const auto &payout: PayoutQueue_)
    enqueued[payout.UserId] += payout.Value;

  bool inconsistent = false;
  for (auto &userIt: BalanceMap_) {
    UInt<384> enqueuedBalance = enqueued[userIt.first];
    if (userIt.second.Requested != enqueuedBalance) {
      LOG_F(ERROR,
            "User %s: enqueued: %s, control sum: %s",
            userIt.first.c_str(),
            FormatMoney(enqueuedBalance, CoinInfo_.FractionalPartSize).c_str(),
            FormatMoney(userIt.second.Requested, CoinInfo_.FractionalPartSize).c_str());
      inconsistent = true;
    }
  }

  if (inconsistent)
    LOG_F(ERROR, "Payout database inconsistent, restart pool for rebuild recommended");

  // Make a service after every payment session
  {
    std::string serviceError;
    if (ClientDispatcher_.ioWalletService(Base_, serviceError) != CNetworkClient::EStatusOk)
      LOG_F(ERROR, "Wallet service ERROR: %s", serviceError.c_str());
  }
}

void CPayoutProcessor::checkBalance(const std::set<MiningRound*> &unpayedRounds, const CPPSState &ppsState)
{
  UInt<384> balance = UInt<384>::zero();
  UInt<384> requestedInBalance = UInt<384>::zero();
  UInt<384> requestedInQueue = UInt<384>::zero();
  UInt<384> confirmationWait = UInt<384>::zero();
  UInt<384> immature = UInt<384>::zero();
  UInt<384> userBalance = UInt<384>::zero();
  UInt<384> queued = UInt<384>::zero();
  UInt<384> ppsPaid = UInt<384>::zero();
  UInt<384> net = UInt<384>::zero();

  UInt<384> zbalance = UInt<384>::zero();
  if (!Cfg_.poolZAddr.empty()) {
    if (ClientDispatcher_.ioZGetBalance(Base_, Cfg_.poolZAddr, &zbalance) != CNetworkClient::EStatusOk) {
      LOG_F(ERROR, "can't get balance of Z-address %s", Cfg_.poolZAddr.c_str());
      return;
    }
  }

  CNetworkClient::GetBalanceResult getBalanceResult;
  if (!ClientDispatcher_.ioGetBalance(Base_, getBalanceResult)) {
    LOG_F(ERROR, "can't retrieve balance");
    return;
  }

  balance = getBalanceResult.Balance + zbalance;
  immature = getBalanceResult.Immatured;

  for (auto &userIt: BalanceMap_) {
    userBalance += userIt.second.Balance;
    requestedInBalance += userIt.second.Requested;
    ppsPaid += userIt.second.PPSPaid;
  }
  for (auto &p: PayoutQueue_) {
    requestedInQueue += p.Value;
    if (p.Status == PayoutDbRecord::ETxSent)
      confirmationWait += p.Value + p.TxFee;
  }

  for (auto &roundIt: unpayedRounds) {
    for (auto &pIt: roundIt->Payouts)
      queued += pIt.Value;
  }
  net = balance + immature - userBalance - queued + confirmationWait + ppsPaid;

  {
    PoolBalanceRecord pb;
    pb.Time = time(0);
    pb.Balance = balance;
    pb.Immature = immature;
    pb.Users = userBalance;
    pb.Queued = queued;
    pb.ConfirmationWait = confirmationWait;
    pb.Net = net;
    PoolBalanceDb_.put(pb);
  }

  LOG_F(INFO,
        "accounting: balance=%s req/balance=%s req/queue=%s immature=%s users=%s queued=%s, confwait=%s, net=%s",
        FormatMoney(balance, CoinInfo_.FractionalPartSize).c_str(),
        FormatMoney(requestedInBalance, CoinInfo_.FractionalPartSize).c_str(),
        FormatMoney(requestedInQueue, CoinInfo_.FractionalPartSize).c_str(),
        FormatMoney(immature, CoinInfo_.FractionalPartSize).c_str(),
        FormatMoney(userBalance, CoinInfo_.FractionalPartSize).c_str(),
        FormatMoney(queued, CoinInfo_.FractionalPartSize).c_str(),
        FormatMoney(confirmationWait, CoinInfo_.FractionalPartSize).c_str(),
        FormatMoney(net, CoinInfo_.FractionalPartSize).c_str());

  if (BackendSettings_.load(std::memory_order_relaxed).PPSConfig.Enabled) {
    const auto &reward = ppsState.LastBaseBlockReward;
    double refBalanceInBlocks = CPPSState::balanceInBlocks(ppsState.ReferenceBalance, reward);
    double refSqLambda = CPPSState::sqLambda(ppsState.ReferenceBalance, reward, ppsState.TotalBlocksFound);
    double minBalanceInBlocks = CPPSState::balanceInBlocks(ppsState.Min.Balance, reward);
    double minSqLambda = CPPSState::sqLambda(ppsState.Min.Balance, reward, ppsState.Min.TotalBlocksFound);
    double maxBalanceInBlocks = CPPSState::balanceInBlocks(ppsState.Max.Balance, reward);
    double maxSqLambda = CPPSState::sqLambda(ppsState.Max.Balance, reward, ppsState.Max.TotalBlocksFound);
    LOG_F(INFO,
          "PPS state: balance=%s, refBalance=%s (%.3f blocks, sqLambda=%.4f),"
          " min=%s (%.3f blocks, sqLambda=%.4f),"
          " max=%s (%.3f blocks, sqLambda=%.4f),"
          " blocks=%.3f, saturateCoeff=%.4f, avgTxFee=%s",
          FormatMoney(ppsState.Balance, CoinInfo_.FractionalPartSize).c_str(),
          FormatMoney(ppsState.ReferenceBalance, CoinInfo_.FractionalPartSize).c_str(),
          refBalanceInBlocks,
          refSqLambda,
          FormatMoney(ppsState.Min.Balance, CoinInfo_.FractionalPartSize).c_str(),
          minBalanceInBlocks,
          minSqLambda,
          FormatMoney(ppsState.Max.Balance, CoinInfo_.FractionalPartSize).c_str(),
          maxBalanceInBlocks,
          maxSqLambda,
          ppsState.TotalBlocksFound,
          ppsState.LastSaturateCoeff,
          FormatMoney(ppsState.LastAverageTxFee, CoinInfo_.FractionalPartSize).c_str());
  }
}

bool CPayoutProcessor::requestManualPayout(const std::string &address)
{
  auto It = BalanceMap_.find(address);
  if (It == BalanceMap_.end())
    return false;

  UserBalanceRecord &balance = It->second;
  auto settingsIt = UserSettings_.find(balance.Login);
  if (settingsIt == UserSettings_.end())
    return false;

  UInt<384> nonQueuedBalance = balance.Balance - balance.Requested;
  if (nonQueuedBalance.isNegative())
    return false;

  PayoutQueue_.push_back(PayoutDbRecord(address, nonQueuedBalance));
  balance.Requested += nonQueuedBalance;
  BalanceDb_.put(balance);
  return true;
}
