#include "poolcore/accountingPayouts.h"
#include "poolcore/accountingState.h"
#include "poolcommon/utils.h"
#include "loguru.hpp"

static const UInt<384> ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE = fromRational(10000u);

CPayoutProcessor::CPayoutProcessor(asyncBase *base,
                                   const PoolBackendConfig &cfg,
                                   const CCoinInfo &coinInfo,
                                   CNetworkClientDispatcher &clientDispatcher,
                                   CAccountingState &state,
                                   kvdb<rocksdbBase> &payoutDb,
                                   kvdb<rocksdbBase> &poolBalanceDb,
                                   const std::unordered_map<std::string, UserSettingsRecord> &userSettings,
                                   const CPriceFetcher &priceFetcher) :
  Base_(base),
  Cfg_(cfg),
  CoinInfo_(coinInfo),
  ClientDispatcher_(clientDispatcher),
  State_(state),
  PayoutDb_(payoutDb),
  PoolBalanceDb_(poolBalanceDb),
  UserSettings_(userSettings),
  PriceFetcher_(priceFetcher)
{
}

void CPayoutProcessor::buildTransaction(PayoutDbRecord &payout, unsigned index, std::string &recipient, bool *needSkipPayout, rocksdbBase::CBatch &batch)
{
  *needSkipPayout = false;
  auto minimalPayout = State_.BackendSettings.load(std::memory_order_relaxed).PayoutConfig.InstantMinimalPayout;
  if (payout.Value < minimalPayout) {
    CLOG_F(INFO,
           "[{}] Accounting: ignore this payout to {}, value is {}, minimal is {}",
           index,
           payout.UserId,
           FormatMoney(payout.Value, CoinInfo_.FractionalPartSize),
           FormatMoney(minimalPayout, CoinInfo_.FractionalPartSize));
    *needSkipPayout = true;
    return;
  }

  // Get address for payment
  auto settingsIt = UserSettings_.find(payout.UserId);
  if (settingsIt == UserSettings_.end() || settingsIt->second.Payout.Address.empty()) {
    CLOG_F(WARNING, "user {} did not setup payout address, ignoring", payout.UserId);
    *needSkipPayout = true;
    return;
  }

  const UserSettingsRecord &settings = settingsIt->second;
  recipient = settings.Payout.Address;
  if (!CoinInfo_.checkAddress(settings.Payout.Address, CoinInfo_.PayoutAddressType)) {
    CLOG_F(ERROR, "Invalid payment address {} for {}", settings.Payout.Address, payout.UserId);
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
    CLOG_F(INFO, "No money left to pay");
    return;
  } else {
    CLOG_F(ERROR, "Payment {} to {} failed with error \"{}\"", FormatMoney(payout.Value, CoinInfo_.FractionalPartSize), settings.Payout.Address, transaction.Error);
    return;
  }

  // int64_t delta = payout.Value - (transaction.Value + transaction.Fee);
  UInt<384> transactionTotalValue = transaction.Value + transaction.Fee;

  if (payout.Value > transactionTotalValue) {
    // Correct payout value and request balance
    UInt<384> delta = payout.Value - transactionTotalValue;
    payout.Value -= delta;

    // Update user balance
    auto It = State_.BalanceMap.find(payout.UserId);
    if (It == State_.BalanceMap.end()) {
      CLOG_F(ERROR, "payout to unknown address {}", payout.UserId);
      return;
    }

    CLOG_F(INFO, "   * correct requested balance for {} by {}", payout.UserId, FormatMoney(delta, CoinInfo_.FractionalPartSize));
    UserBalanceRecord &balance = It->second;
    balance.Requested -= delta;
    State_.putBalance(batch, balance);
  } else if (payout.Value < transactionTotalValue) {
    CLOG_F(ERROR, "Payment {} to {} failed: too big transaction amount", FormatMoney(payout.Value, CoinInfo_.FractionalPartSize), settings.Payout.Address);
    return;
  }

  // Save transaction to database
  if (!State_.KnownTransactions.insert(transaction.TxId).second) {
    CLOG_F(ERROR, "Node generated duplicate for transaction {} !!!", transaction.TxId);
    return;
  }

  payout.TransactionData = transaction.TxData;
  payout.TransactionId = transaction.TxId;
  payout.Time = Timestamp::now();
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
    CLOG_F(ERROR, "Transaction {} to {} marked as rejected, removing from database...", payout.TransactionId, payout.UserId);

    // Update transaction in database
    payout.Status = PayoutDbRecord::ETxRejected;
    PayoutDb_.put(payout);

    // Clear all data and re-schedule payout
    payout.TransactionId.clear();
    payout.TransactionData.clear();
    payout.Status = PayoutDbRecord::EInitialized;
    return false;
  } else {
    CLOG_F(WARNING, "Sending transaction {} to {} error \"{}\", will try send later...", payout.TransactionId, payout.UserId, error);
    return false;
  }

  payout.Status = PayoutDbRecord::ETxSent;
  PayoutDb_.put(payout);
  return true;
}

bool CPayoutProcessor::checkTxConfirmations(PayoutDbRecord &payout, rocksdbBase::CBatch &batch)
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
    CLOG_F(ERROR, "Transaction {} to {} marked as rejected, removing from database...", payout.TransactionId, payout.UserId);

    // Update transaction in database
    payout.Status = PayoutDbRecord::ETxRejected;
    PayoutDb_.put(payout);

    // Clear all data and re-schedule payout
    payout.TransactionId.clear();
    payout.TransactionData.clear();
    payout.Status = PayoutDbRecord::EInitialized;
    return false;
  } else {
    CLOG_F(WARNING, "Checking transaction {} to {} error \"{}\", will do it later...", payout.TransactionId, payout.UserId, error);
    return false;
  }

  // Update database
  if (confirmations >= Cfg_.RequiredConfirmations) {
    payout.Status = PayoutDbRecord::ETxConfirmed;
    PayoutDb_.put(payout);

    // Update user balance
    auto It = State_.BalanceMap.find(payout.UserId);
    if (It == State_.BalanceMap.end()) {
      CLOG_F(ERROR, "payout to unknown address {}", payout.UserId);
      return false;
    }

    UserBalanceRecord &balance = It->second;
    // Balance can become negative (unsigned underflow) if fees exceed expectations.
    // This is handled: requestPayout/manualPayoutImpl check isNegative() before queuing new payouts.
    balance.Balance -= (payout.Value + payout.TxFee);
    balance.Requested -= payout.Value;
    balance.Paid += payout.Value;
    State_.putBalance(batch, balance);
    return true;
  }

  return false;
}

void CPayoutProcessor::makePayout()
{
  if (!State_.PayoutQueue.empty()) {
    CLOG_F(INFO, "Accounting: checking {} payout requests...", State_.PayoutQueue.size());

    auto batch = CAccountingState::batch();

    // Merge small payouts and payouts to invalid address
    // TODO: merge small payouts with normal also
    {
      std::map<std::string, UInt<384>> payoutAccMap;
      for (auto I = State_.PayoutQueue.begin(), IE = State_.PayoutQueue.end(); I != IE;) {
        if (I->Status != PayoutDbRecord::EInitialized) {
          ++I;
          continue;
        }

        if (I->Value < State_.BackendSettings.load(std::memory_order_relaxed).PayoutConfig.InstantMinimalPayout) {
          payoutAccMap[I->UserId] += I->Value;
          CLOG_F(INFO,
                 "Accounting: merge payout {} for {} (total already {})",
                 FormatMoney(I->Value, CoinInfo_.FractionalPartSize),
                 I->UserId,
                 FormatMoney(payoutAccMap[I->UserId], CoinInfo_.FractionalPartSize));
          State_.PayoutQueue.erase(I++);
        } else {
          ++I;
        }
      }

      for (const auto &I: payoutAccMap) {
        PayoutDbRecord payoutRecord(I.first, I.second);
        payoutRecord.RateToBTC = PriceFetcher_.getPrice(CoinInfo_.Name);
        payoutRecord.RateBTCToUSD = PriceFetcher_.getBtcUsd();
        State_.PayoutQueue.push_back(std::move(payoutRecord));
      }
    }

    unsigned index = 0;
    for (auto &payout: State_.PayoutQueue) {
      if (payout.Status == PayoutDbRecord::EInitialized) {
        // Build transaction
        // For bitcoin-based API it's sequential call of createrawtransaction, fundrawtransaction and signrawtransaction
        bool needSkipPayout;
        std::string recipientAddress;
        buildTransaction(payout, index, recipientAddress, &needSkipPayout, batch);
        if (needSkipPayout)
          continue;

        if (payout.Status == PayoutDbRecord::ETxCreated) {
          // Persist TransactionId before sending to prevent duplicate transactions on crash
          State_.addPayoutQueue(batch);
          State_.flushState(batch);
          batch = CAccountingState::batch();

          // Send transaction and change it status to 'Sent'
          // For bitcoin-based API it's 'sendrawtransaction'
          if (sendTransaction(payout))
            CLOG_F(INFO, " * sent {} to {}({}) with txid {}", FormatMoney(payout.Value, CoinInfo_.FractionalPartSize), payout.UserId, recipientAddress, payout.TransactionId);
        } else {
          // buildTransaction failed — stop processing the queue;
          // this payout will block all subsequent payouts until resolved
          break;
        }
      } else if (payout.Status == PayoutDbRecord::ETxCreated) {
        // Resend transaction
        if (sendTransaction(payout))
          CLOG_F(INFO, " * retry send txid {} to {}", payout.TransactionId, payout.UserId);
      } else if (payout.Status == PayoutDbRecord::ETxSent) {
        // Check confirmations
        if (checkTxConfirmations(payout, batch))
          CLOG_F(INFO, " * transaction txid {} to {} confirmed", payout.TransactionId, payout.UserId);
      } else {
        // Invalid status
      }
    }

    // Cleanup confirmed payouts
    for (auto I = State_.PayoutQueue.begin(), IE = State_.PayoutQueue.end(); I != IE;) {
      if (I->Status == PayoutDbRecord::ETxConfirmed) {
        State_.KnownTransactions.erase(I->TransactionId);
        State_.PayoutQueue.erase(I++);
      } else {
        ++I;
      }
    }

    State_.addPayoutQueue(batch);
    State_.flushState(batch);
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
          CLOG_F(INFO,
                 " * moving {} coins from {} to {} started ({})",
                 FormatMoney(out.second, CoinInfo_.FractionalPartSize),
                 out.first,
                 Cfg_.poolZAddr,
                 zsendResult.AsyncOperationId);
        } else {
          CLOG_F(INFO,
                 " * async operation start error {}: source={}, destination={}, amount={}",
                 !zsendResult.Error.empty() ? zsendResult.Error : std::string("<unknown error>"),
                 out.first,
                 Cfg_.poolZAddr,
                 FormatMoney(out.second, CoinInfo_.FractionalPartSize));
        }
      }
    }

    // move Z-Addr to T-Addr
    UInt<384> zbalance;
    if (ClientDispatcher_.ioZGetBalance(Base_, Cfg_.poolZAddr, &zbalance) == CNetworkClient::EStatusOk && zbalance.nonZero()) {
      CLOG_F(INFO, "Accounting: move {} coins to transparent address", FormatMoney(zbalance, CoinInfo_.FractionalPartSize));
      CNetworkClient::ZSendMoneyResult zsendResult;
      if (ClientDispatcher_.ioZSendMoney(Base_, Cfg_.poolZAddr, Cfg_.poolTAddr, zbalance, "", 1, UInt<384>::zero(), zsendResult) == CNetworkClient::EStatusOk) {
        CLOG_F(INFO,
               "moving {} coins from {} to {} started ({})",
               FormatMoney(zbalance, CoinInfo_.FractionalPartSize),
               Cfg_.poolZAddr,
               Cfg_.poolTAddr,
               !zsendResult.AsyncOperationId.empty() ? zsendResult.AsyncOperationId : std::string("<none>"));
      }
    }
  }

  // Check consistency
  std::unordered_map<std::string, UInt<384>> enqueued;
  for (const auto &payout: State_.PayoutQueue)
    enqueued[payout.UserId] += payout.Value;

  bool inconsistent = false;
  for (auto &userIt: State_.BalanceMap) {
    UInt<384> enqueuedBalance = enqueued[userIt.first];
    if (userIt.second.Requested != enqueuedBalance) {
      CLOG_F(ERROR,
             "User {}: enqueued: {}, control sum: {}",
             userIt.first,
             FormatMoney(enqueuedBalance, CoinInfo_.FractionalPartSize),
             FormatMoney(userIt.second.Requested, CoinInfo_.FractionalPartSize));
      inconsistent = true;
    }
  }

  if (inconsistent)
    CLOG_F(ERROR, "Payout database inconsistent, restart pool for rebuild recommended");

  // Make a service after every payment session
  {
    std::string serviceError;
    if (ClientDispatcher_.ioWalletService(Base_, serviceError) != CNetworkClient::EStatusOk)
      CLOG_F(ERROR, "Wallet service ERROR: {}", serviceError);
  }

}

void CPayoutProcessor::checkBalance()
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
      CLOG_F(ERROR, "can't get balance of Z-address {}", Cfg_.poolZAddr);
      return;
    }
  }

  CNetworkClient::GetBalanceResult getBalanceResult;
  if (!ClientDispatcher_.ioGetBalance(Base_, getBalanceResult)) {
    CLOG_F(ERROR, "can't retrieve balance");
    return;
  }

  balance = getBalanceResult.Balance + zbalance;
  immature = getBalanceResult.Immatured;

  for (auto &userIt: State_.BalanceMap) {
    userBalance += userIt.second.Balance;
    requestedInBalance += userIt.second.Requested;
    ppsPaid += userIt.second.PPSPaid;
  }
  for (auto &p: State_.PayoutQueue) {
    requestedInQueue += p.Value;
    if (p.Status == PayoutDbRecord::ETxSent)
      confirmationWait += p.Value + p.TxFee;
  }

  for (auto &round: State_.ActiveRounds) {
    for (auto &pIt: round.Payouts)
      queued += pIt.Value;
  }
  net = balance + immature - userBalance - queued + confirmationWait + ppsPaid;

  {
    PoolBalanceRecord pb;
    pb.Time = Timestamp::now();
    pb.Balance = balance;
    pb.Immature = immature;
    pb.Users = userBalance;
    pb.Queued = queued;
    pb.ConfirmationWait = confirmationWait;
    pb.Net = net;
    PoolBalanceDb_.put(pb);
  }

  CLOG_F(INFO,
         "accounting: balance={} req/balance={} req/queue={} immature={} users={} queued={}, confwait={}, net={}",
         FormatMoney(balance, CoinInfo_.FractionalPartSize),
         FormatMoney(requestedInBalance, CoinInfo_.FractionalPartSize),
         FormatMoney(requestedInQueue, CoinInfo_.FractionalPartSize),
         FormatMoney(immature, CoinInfo_.FractionalPartSize),
         FormatMoney(userBalance, CoinInfo_.FractionalPartSize),
         FormatMoney(queued, CoinInfo_.FractionalPartSize),
         FormatMoney(confirmationWait, CoinInfo_.FractionalPartSize),
         FormatMoney(net, CoinInfo_.FractionalPartSize));

  if (State_.BackendSettings.load(std::memory_order_relaxed).PPSConfig.Enabled) {
    const auto &reward = State_.PPSState.LastBaseBlockReward;
    double refBalanceInBlocks = CPPSState::balanceInBlocks(State_.PPSState.ReferenceBalance, reward);
    double refSqLambda = CPPSState::sqLambda(State_.PPSState.ReferenceBalance, reward, State_.PPSState.TotalBlocksFound);
    double minBalanceInBlocks = CPPSState::balanceInBlocks(State_.PPSState.Min.Balance, reward);
    double minSqLambda = CPPSState::sqLambda(State_.PPSState.Min.Balance, reward, State_.PPSState.Min.TotalBlocksFound);
    double maxBalanceInBlocks = CPPSState::balanceInBlocks(State_.PPSState.Max.Balance, reward);
    double maxSqLambda = CPPSState::sqLambda(State_.PPSState.Max.Balance, reward, State_.PPSState.Max.TotalBlocksFound);
    CLOG_F(INFO,
           "PPS state: balance={}, refBalance={} ({:.3f} blocks, sqLambda={:.4f}),"
           " min={} ({:.3f} blocks, sqLambda={:.4f}),"
           " max={} ({:.3f} blocks, sqLambda={:.4f}),"
           " blocks={:.3f}, saturateCoeff={:.4f}, avgTxFee={}",
           FormatMoney(State_.PPSState.Balance, CoinInfo_.FractionalPartSize),
           FormatMoney(State_.PPSState.ReferenceBalance, CoinInfo_.FractionalPartSize),
           refBalanceInBlocks,
           refSqLambda,
           FormatMoney(State_.PPSState.Min.Balance, CoinInfo_.FractionalPartSize),
           minBalanceInBlocks,
           minSqLambda,
           FormatMoney(State_.PPSState.Max.Balance, CoinInfo_.FractionalPartSize),
           maxBalanceInBlocks,
           maxSqLambda,
           State_.PPSState.TotalBlocksFound,
           State_.PPSState.LastSaturateCoeff,
           FormatMoney(State_.PPSState.LastAverageTxFee, CoinInfo_.FractionalPartSize));
  }
}

bool CPayoutProcessor::requestManualPayout(const std::string &address, rocksdbBase::CBatch &batch)
{
  auto It = State_.BalanceMap.find(address);
  if (It == State_.BalanceMap.end())
    return false;

  UserBalanceRecord &balance = It->second;
  auto settingsIt = UserSettings_.find(balance.Login);
  if (settingsIt == UserSettings_.end())
    return false;

  UInt<384> nonQueuedBalance = balance.Balance - balance.Requested;
  if (nonQueuedBalance.isNegative())
    return false;

  PayoutDbRecord payoutRecord(address, nonQueuedBalance);
  payoutRecord.RateToBTC = PriceFetcher_.getPrice(CoinInfo_.Name);
  payoutRecord.RateBTCToUSD = PriceFetcher_.getBtcUsd();
  State_.PayoutQueue.push_back(std::move(payoutRecord));
  balance.Requested += nonQueuedBalance;
  State_.putBalance(batch, balance);
  return true;
}
