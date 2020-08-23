#include "poolcore/accounting.h"
#include "poolcommon/utils.h"
#include "poolcore/base58.h"
#include "poolcore/statistics.h"
#include "loguru.hpp"
#include <stdarg.h>
#include <poolcommon/file.h>

#define ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE   10000


AccountingDb::AccountingDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo, UserManager &userMgr, CNetworkClientDispatcher &clientDispatcher) :
  Base_(base),
  _cfg(config),
  CoinInfo_(coinInfo),
  UserManager_(userMgr),
  ClientDispatcher_(clientDispatcher),
  _roundsDb(config.dbPath / "rounds"),
  _balanceDb(config.dbPath / "balance"),
  _foundBlocksDb(config.dbPath / "foundBlocks"),
  _poolBalanceDb(config.dbPath / "poolBalance"),
  _payoutDb(config.dbPath / "payouts")
{
  {
    unsigned counter = 0;
    _sharesFd.open(_cfg.dbPath / "shares.raw");
    if (!_sharesFd.isOpened())
      LOG_F(ERROR, "can't open shares file %s (%s)", (_cfg.dbPath / "shares.raw").u8string().c_str(), strerror(errno));

    auto fileSize = _sharesFd.size();
    if (fileSize > 0) {
      xmstream stream;
      _sharesFd.read(stream.reserve(fileSize), 0, fileSize);

      stream.seekSet(0);
      while (stream.remaining()) {
        uint32_t userIdSize = stream.read<uint32_t>();
        const char *userIdData = stream.seek<const char>(userIdSize);
        if (!userIdData)
          break;
        int64_t value = stream.read<int64_t>();
        if (stream.eof())
          break;
        _currentScores[std::string(userIdData, userIdSize)] += value;
        counter++;
      }
    }

    LOG_F(INFO, "loaded %u shares from shares.raw file", counter);
    for (auto s: _currentScores)
      LOG_F(INFO, "   * %s: %" PRId64 "", s.first.c_str(), s.second);

    _sharesFd.seekSet(fileSize);
  }

  {
    FileDescriptor payoutsFdOld;
    payoutsFdOld.open(_cfg.dbPath / "payouts.raw.old");
    if (!payoutsFdOld.isOpened())
      LOG_F(ERROR, "can't open payouts file %s (%s)", (_cfg.dbPath / "payouts.raw.old").u8string().c_str(), strerror(errno));

    auto fileSize = payoutsFdOld.size();

    if (fileSize > 0) {
      xmstream stream;
      payoutsFdOld.read(stream.reserve(fileSize), 0, fileSize);

      stream.seekSet(0);
      while (stream.remaining()) {
        uint32_t userIdSize = stream.read<uint32_t>();
        const char *userIdData = stream.seek<const char>(userIdSize);
        if (!userIdData)
          break;
        int64_t value = stream.read<int64_t>();
        if (stream.eof())
          break;
        _payoutQueue.push_back(payoutElement(std::string(userIdData, userIdSize), value, 0));
      }
    }

    payoutsFdOld.truncate(0);
    LOG_F(INFO, "loaded %u payouts from payouts.raw.old file", (unsigned)_payoutQueue.size());
  }

  {
    unsigned payoutsNum = 0;
    _payoutsFd.open(_cfg.dbPath / "payouts.raw");
    if (!_payoutsFd.isOpened())
      LOG_F(ERROR, "can't open payouts file %s (%s)", (_cfg.dbPath / "payouts.raw").u8string().c_str(), strerror(errno));

    auto fileSize = _payoutsFd.size();
    if (fileSize > 0) {
      xmstream stream;
      _payoutsFd.read(stream.reserve(fileSize), 0, fileSize);

      stream.seekSet(0);
      while (stream.remaining()) {
        payoutElement element;
        if (!element.deserializeValue(stream))
          break;
        _payoutQueue.push_back(element);
        payoutsNum++;
      }
    }

    LOG_F(INFO, "loaded %u payouts from payouts.raw file", payoutsNum);
    if (payoutsNum != _payoutQueue.size())
      updatePayoutFile();
  }

  {
    std::unique_ptr<rocksdbBase::IteratorType> It(_roundsDb.iterator());
    It->seekFirst();
    for (; It->valid(); It->next()) {
      miningRound *R = new miningRound;
      RawData data = It->value();
      if (R->deserializeValue(data.data, data.size)) {
        _allRounds.push_back(R);
        for (auto p: R->payouts) {
          if (p.queued) {
            _roundsWithPayouts.insert(R);
            break;
          }
        }
      } else {
        LOG_F(ERROR, "rounds db contains invalid record");
        delete R;
      }
    }

    LOG_F(INFO, "loaded %u rounds from db", (unsigned)_allRounds.size());
  }

  {
    std::unique_ptr<rocksdbBase::IteratorType> It(_balanceDb.iterator());
    It->seekFirst();
    for (; It->valid(); It->next()) {
      UserBalanceRecord ub;
      RawData data = It->value();
      if (ub.deserializeValue(data.data, data.size))
        _balanceMap[ub.Login] = ub;
    }

    LOG_F(INFO, "loaded %u user balance data from db", (unsigned)_balanceMap.size());
  }
}

void AccountingDb::updatePayoutFile()
{
  xmstream stream;
  for (auto &p: _payoutQueue)
    p.serializeValue(stream);

  _payoutsFd.write(stream.data(), 0, stream.sizeOf());
  _payoutsFd.truncate(stream.sizeOf());
}

void AccountingDb::cleanupRounds()
{
  time_t timeLabel = time(0) - _cfg.KeepRoundTime;
  auto I = _allRounds.begin();
  while (I != _allRounds.end()) {
    if ((*I)->time >= timeLabel || _roundsWithPayouts.count(*I) > 0)
      break;
    _roundsDb.deleteRow(**I);
    ++I;
  }

  if (I != _allRounds.begin()) {
    LOG_F(INFO, "delete %u old rounds", (unsigned)std::distance(_allRounds.begin(), I));
    _allRounds.erase(_allRounds.begin(), I);
  }
}

void AccountingDb::addShare(const CShare &share)
{
  // store share in shares.raw file
  {
    xmstream stream;
    stream.write<uint32_t>(static_cast<uint32_t>(share.userId.size()));
    stream.write(share.userId.c_str(), share.userId.size());
    stream.write<int64_t>(share.value);
    if (_sharesFd.write(stream.data(), stream.sizeOf() != stream.sizeOf()))
      LOG_F(ERROR, "can't save share to file (%s fd=%i), it can be lost", strerror(errno), _sharesFd.fd());
  }

  // increment score
  _currentScores[share.userId.c_str()] += share.value;

  if (share.isBlock) {
    {
      // save to database
      FoundBlockRecord blk;
      blk.Height = share.height;
      blk.Hash = share.hash.c_str();
      blk.Time = time(0);
      blk.AvailableCoins = share.generatedCoins;
      blk.FoundBy = share.userId.c_str();
      _foundBlocksDb.put(blk);
    }

    int64_t generatedCoins = share.generatedCoins;
    if (!_cfg.poolZAddr.empty()) {
      // calculate miners fee for Z-Addr moving
      generatedCoins -= (2*ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE);
    }

    int64_t feeValuesSum = 0;
    std::vector<int64_t> feeValues;
    for (const auto &poolFeeRecord: _cfg.PoolFee) {
      int64_t value = static_cast<int64_t>(generatedCoins * (poolFeeRecord.Percentage / 100.0f));
      feeValues.push_back(value);
      feeValuesSum += value;
    }

    int64_t available = generatedCoins - feeValuesSum;
    LOG_F(INFO, " * block height: %u, hash: %s, value: %" PRId64 ", pool fee: %" PRIu64 ", available: %" PRIu64 "", (unsigned)share.height, share.hash.c_str(), generatedCoins, feeValuesSum, available);

    miningRound *R = new miningRound;
    R->height = share.height;
    R->blockHash = share.hash.c_str();
    R->time = time(0);
    R->availableCoins = available;
    R->totalShareValue = 0;

    {
      for (auto It: _currentScores) {
        roundElement element;
        element.userId = It.first;
        element.shareValue = It.second;
        R->rounds.push_back(element);
        R->totalShareValue += element.shareValue;
      }

      _currentScores.clear();
    }

    // *** calculate payments ***
    {
      // insert round into accounting base, keep sorting by height
      auto position = std::lower_bound(
        _allRounds.begin(),
        _allRounds.end(),
        R->height,
        [](const miningRound *roundArg, uint64_t heightArg) -> bool { return roundArg->height < heightArg; });
      auto insertIt = _allRounds.insert(position, R);
      std::deque<miningRound*>::reverse_iterator roundIt(++insertIt);

      // calculate shares for each client using last N rounds
      time_t previousRoundTime = R->time;
      int64_t totalValue = 0;
      std::list<payoutAggregate> agg;
      for (size_t i = 0, ie = _cfg.PoolFee.size(); i != ie; ++i) {
        agg.push_back(payoutAggregate(_cfg.PoolFee[0].User, 0));
        agg.front().payoutValue = feeValues[i];
      }

      while (roundIt != _allRounds.rend() && ((R->time - previousRoundTime) < 3600)) {
        // merge sorted by userId lists
        auto currentRound = *roundIt;
        auto aggIt = agg.begin();
        auto shareIt = currentRound->rounds.begin();
        while (shareIt != currentRound->rounds.end()) {
          if (aggIt == agg.end() || shareIt->userId < aggIt->userId) {
            agg.insert(aggIt, payoutAggregate(shareIt->userId, shareIt->shareValue));
            ++shareIt;
          } else if (shareIt->userId == aggIt->userId) {
            aggIt->shareValue += shareIt->shareValue;
            ++shareIt;
            ++aggIt;
          } else {
            ++aggIt;
          }
        }

        previousRoundTime = (*roundIt)->time;
        totalValue += currentRound->totalShareValue;
        ++roundIt;
      }

      // calculate payout values for each client
      int64_t totalPayout = 0;
      LOG_F(INFO, " * total share value: %" PRId64 "", totalValue);
      for (auto I = agg.begin(), IE = agg.end(); I != IE; ++I) {
        I->payoutValue += static_cast<int64_t>((static_cast<double>(I->shareValue) / static_cast<double>(totalValue)) * R->availableCoins);
        totalPayout += I->payoutValue;
        LOG_F(INFO, "   * addr: %s, payout: %" PRId64 "", I->userId.c_str(), I->payoutValue);
      }

      LOG_F(INFO, " * total payout: %" PRId64 "", totalPayout);

      // correct payouts for use all available coins
      if (!agg.empty()) {
        int64_t diff = totalPayout - generatedCoins;
        int64_t div = diff / (int64_t)agg.size();
        int64_t mv = diff >= 0 ? 1 : -1;
        int64_t mod = (diff > 0 ? diff : -diff) % agg.size();

        totalPayout = 0;
        int64_t i = 0;
        for (auto I = agg.begin(), IE = agg.end(); I != IE; ++I, ++i) {
          I->payoutValue -= div;
          if (i < mod)
            I->payoutValue -= mv;
          totalPayout += I->payoutValue;
        }

        LOG_F(INFO, " * total payout (after correct): %" PRId64 "", totalPayout);
      }

      // calculate payout delta for each user and send to queue
      // merge sorted by userId lists
      auto aggIt = agg.begin();
      auto payIt = R->payouts.begin();
      while (aggIt != agg.end()) {
        if (payIt == R->payouts.end() || aggIt->userId < payIt->Login) {
          R->payouts.insert(payIt, payoutElement(aggIt->userId, aggIt->payoutValue, aggIt->payoutValue));
          _roundsWithPayouts.insert(R);
          ++aggIt;
        } else if (aggIt->userId == payIt->Login) {
          int64_t delta = aggIt->payoutValue - payIt->payoutValue;
          if (delta) {
            payIt->queued += delta;
            _roundsWithPayouts.insert(R);
          }
          payIt->payoutValue = aggIt->payoutValue;
          ++aggIt;
          ++payIt;
        } else {
          ++payIt;
        }
      }
    }

    // store round to DB and clear shares map
    _roundsDb.put(*R);
    _sharesFd.truncate(0);
  }
}

void AccountingDb::mergeRound(const Round*)
{
}

void AccountingDb::checkBlockConfirmations()
{
  if (_roundsWithPayouts.empty())
    return;

  LOG_F(INFO, "Checking %u blocks for confirmations...", (unsigned)_roundsWithPayouts.size());
  std::vector<miningRound*> rounds(_roundsWithPayouts.begin(), _roundsWithPayouts.end());

  std::vector<CNetworkClient::GetBlockConfirmationsQuery> confirmationsQuery(rounds.size());
  for (size_t i = 0, ie = rounds.size(); i != ie; ++i) {
    confirmationsQuery[i].Hash = rounds[i]->blockHash;
    confirmationsQuery[i].Height = rounds[i]->height;
  }

  if (!ClientDispatcher_.ioGetBlockConfirmations(Base_, confirmationsQuery)) {
    LOG_F(ERROR, "ioGetBlockConfirmations api call failed");
    return;
  }

  for (size_t i = 0; i < confirmationsQuery.size(); i++) {
    miningRound *R = rounds[i];

    if (confirmationsQuery[i].Confirmations == -1) {
      LOG_F(INFO, "block %" PRIu64 "/%s marked as orphan, can't do any payout", R->height, confirmationsQuery[i].Hash.c_str());
      for (auto I = R->payouts.begin(), IE = R->payouts.end(); I != IE; ++I)
        I->queued = 0;
      _roundsWithPayouts.erase(R);
      _roundsDb.put(*R);
    } else if (confirmationsQuery[i].Confirmations >= _cfg.RequiredConfirmations) {
      LOG_F(INFO, "Make payout for block %" PRIu64 "/%s", R->height, R->blockHash.c_str());
      for (auto I = R->payouts.begin(), IE = R->payouts.end(); I != IE; ++I) {
        requestPayout(I->Login, I->queued);
        I->queued = 0;
      }
      _roundsWithPayouts.erase(R);
      _roundsDb.put(*R);
    }
  }

  updatePayoutFile();
}

void AccountingDb::makePayout()
{
  if (!_payoutQueue.empty()) {
    LOG_F(INFO, "Accounting: checking %u payout requests...", (unsigned)_payoutQueue.size());

    // Merge small payouts and payouts to invalid address
    {
      std::map<std::string, int64_t> payoutAccMap;
      for (auto I = _payoutQueue.begin(), IE = _payoutQueue.end(); I != IE;) {
        if (I->payoutValue < _cfg.MinimalAllowedPayout) {
          payoutAccMap[I->Login] += I->payoutValue;
          LOG_F(INFO,
                "Accounting: merge payout %s for %s (total already %s)",
                FormatMoney(I->payoutValue, CoinInfo_.RationalPartSize).c_str(),
                I->Login.c_str(),
                FormatMoney(payoutAccMap[I->Login], CoinInfo_.RationalPartSize).c_str());
          _payoutQueue.erase(I++);
        } else {
          ++I;
        }
      }

      for (auto I: payoutAccMap)
        _payoutQueue.push_back(payoutElement(I.first, I.second, 0));
    }

    unsigned index = 0;
    for (auto I = _payoutQueue.begin(), IE = _payoutQueue.end(); I != IE;) {
      if (I->payoutValue < _cfg.MinimalAllowedPayout) {
        LOG_F(INFO,
              "[%u] Accounting: ignore this payout to %s, value is %s, minimal is %s",
              index,
              I->Login.c_str(),
              FormatMoney(I->payoutValue, CoinInfo_.RationalPartSize).c_str(),
              FormatMoney(_cfg.MinimalAllowedPayout, CoinInfo_.RationalPartSize).c_str());
        ++I;
        continue;
      }

      // Get address for payment
      UserSettingsRecord settings;
      bool hasSettings = UserManager_.getUserCoinSettings(I->Login, CoinInfo_.Name, settings);
      if (!hasSettings || settings.Address.empty()) {
        LOG_F(WARNING, "user %s did not setup payout address, ignoring", I->Login.c_str());
        ++I;
        continue;
      }

      if (!CoinInfo_.checkAddress(settings.Address, CoinInfo_.PayoutAddressType)) {
        LOG_F(ERROR, "Invalid payment address %s for %s", settings.Address.c_str(), I->Login.c_str());
        ++I;
        continue;
      }

      CNetworkClient::SendMoneyResult result;
      if (!ClientDispatcher_.ioSendMoney(Base_, settings.Address.c_str(), I->payoutValue, result)) {
        LOG_F(ERROR,
              "Accounting: [%u] SendMoneyToDestination FAILED: %s (%s) coins to %s because: %s",
              index,
              FormatMoney(I->payoutValue, CoinInfo_.RationalPartSize).c_str(),
              I->Login.c_str(),
              settings.Address.c_str(),
              result.Error.c_str());
        if (result.Error == "Insufficient funds") {
          LOG_F(WARNING, "[%u] Accounting: no money left to pay", index);
          break;
        } else {
          ++I;
          continue;
        }
      }

      LOG_F(INFO,
            "Accounting: [%u] %s coins sent to %s (%s) with txid %s",
            index,
            FormatMoney(I->payoutValue, CoinInfo_.RationalPartSize).c_str(),
            I->Login.c_str(),
            settings.Address.c_str(),
            result.TxId.c_str());
      payoutSuccess(I->Login, I->payoutValue, result.Fee, result.TxId.c_str());
      _payoutQueue.erase(I++);
      index++;
    }

    updatePayoutFile();
  }

  if (!_cfg.poolZAddr.empty() && !_cfg.poolTAddr.empty()) {
    // move all to Z-Addr
    //    auto unspent = ioListUnspent(_client);
    //    if (unspent && !unspent->outs.empty()) {
    CNetworkClient::ListUnspentResult unspent;
    if (ClientDispatcher_.ioListUnspent(Base_, unspent) && !unspent.Outs.empty()) {

      LOG_F(INFO, "Accounting: move %zu coinbase outs to Z-Addr", unspent.Outs.size());
      for (const auto &out: unspent.Outs) {
        if (out.Address == _cfg.poolTAddr || out.Amount < ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE)
          continue;

        CNetworkClient::ZSendMoneyResult zsendResult;
        if (!ClientDispatcher_.ioZSendMoney(Base_, out.Address, _cfg.poolZAddr, out.Amount, "", zsendResult) || zsendResult.AsyncOperationId.empty()) {
          LOG_F(INFO,
                "async operation start error %s: source=%s, destination=%s, amount=%li",
                !zsendResult.Error.empty() ? zsendResult.Error.c_str() : "<unknown error>",
                out.Address.c_str(),
                _cfg.poolZAddr.c_str(),
                (long)out.Amount);
          continue;
        }

        LOG_F(INFO,
              "moving %li coins from %s to %s started (%s)",
              (long)out.Amount,
              out.Address.c_str(),
              _cfg.poolZAddr.c_str(),
              zsendResult.AsyncOperationId.c_str());
      }
    }

    // move Z-Addr to T-Addr
    int64_t zbalance;
    if (ClientDispatcher_.ioZGetBalance(Base_, &zbalance) && zbalance > 0) {

      LOG_F(INFO, "<info> Accounting: move %.3lf coins to transparent address", zbalance/100000000.0);
      CNetworkClient::ZSendMoneyResult zsendResult;
      if (ClientDispatcher_.ioZSendMoney(Base_, _cfg.poolZAddr, _cfg.poolTAddr, zbalance - ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE, "", zsendResult)) {
        LOG_F(INFO,
              "moving %li coins from %s to %s started (%s)",
              (long)(zbalance - ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE),
              _cfg.poolZAddr.c_str(),
              _cfg.poolTAddr.c_str(),
              !zsendResult.AsyncOperationId.empty() ? zsendResult.AsyncOperationId.c_str() : "<none>");
      }
    }
  }

  // Check consistency
  std::set<std::string> waitingForPayout;
  for (auto &p: _payoutQueue)
    waitingForPayout.insert(p.Login);

  int64_t totalLost = 0;
  for (auto &userIt: _balanceMap) {
    if (userIt.second.Requested > 0 && waitingForPayout.count(userIt.second.Login) == 0) {
      // payout lost? add to back of queue
      LOG_F(WARNING, "found lost payout! %s", userIt.second.Login.c_str());
       _payoutQueue.push_back(payoutElement(userIt.second.Login, userIt.second.Requested, 0));
       totalLost = userIt.second.Requested;
    }
  }

  if (totalLost) {
    LOG_F(WARNING, "total lost: %li", (long)totalLost);
    updatePayoutFile();
  }
}

void AccountingDb::checkBalance()
{
  int64_t balance = 0;
  int64_t requestedInBalance = 0;
  int64_t requestedInQueue = 0;
  int64_t immature = 0;
  int64_t userBalance = 0;
  int64_t queued = 0;
  int64_t net = 0;

  int64_t zbalance = 0;
  if (!_cfg.poolZAddr.empty()) {
    if (!ClientDispatcher_.ioZGetBalance(Base_, &zbalance)) {
      LOG_F(ERROR, "can't get balance of Z-address %s", _cfg.poolZAddr.c_str());
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

  for (auto &userIt: _balanceMap) {
    userBalance += userIt.second.Balance+userIt.second.Requested;
    requestedInBalance += userIt.second.Requested;
  }

  for (auto &p: _payoutQueue)
    requestedInQueue += p.payoutValue;

  for (auto &roundIt: _roundsWithPayouts) {
    for (auto &pIt: roundIt->payouts)
      queued += pIt.queued;
  }

  net = balance + immature - userBalance - queued;

  {
    PoolBalanceRecord pb;
    pb.Time = time(0);
    pb.Balance = balance;
    pb.Immature = immature;
    pb.Users = userBalance;
    pb.Queued = queued;
    pb.Net = net;
    _poolBalanceDb.put(pb);
  }

  LOG_F(INFO,
        "accounting: balance=%s req/balance=%s req/queue=%s immature=%s users=%s queued=%s, net=%s",
        FormatMoney(balance, CoinInfo_.RationalPartSize).c_str(),
        FormatMoney(requestedInBalance, CoinInfo_.RationalPartSize).c_str(),
        FormatMoney(requestedInQueue, CoinInfo_.RationalPartSize).c_str(),
        FormatMoney(immature, CoinInfo_.RationalPartSize).c_str(),
        FormatMoney(userBalance, CoinInfo_.RationalPartSize).c_str(),
        FormatMoney(queued, CoinInfo_.RationalPartSize).c_str(),
        FormatMoney(net, CoinInfo_.RationalPartSize).c_str());
}

void AccountingDb::requestPayout(const std::string &address, int64_t value, bool force)
{
  auto It = _balanceMap.find(address);
  if (It == _balanceMap.end())
    It = _balanceMap.insert(It, std::make_pair(address, UserBalanceRecord(address, _cfg.DefaultPayoutThreshold)));

  UserBalanceRecord &balance = It->second;
  balance.Balance += value;

  UserSettingsRecord settings;
  bool hasSettings = UserManager_.getUserCoinSettings(balance.Login, CoinInfo_.Name, settings);
  if (force || (hasSettings && settings.AutoPayout && balance.Balance >= settings.MinimalPayout)) {
    _payoutQueue.push_back(payoutElement(address, balance.Balance, 0));
    balance.Requested += balance.Balance;
    balance.Balance = 0;
  }

  _balanceDb.put(balance);
}

void AccountingDb::payoutSuccess(const std::string &address, int64_t value, int64_t fee, const std::string &transactionId)
{
  auto It = _balanceMap.find(address);
  if (It == _balanceMap.end()) {
    LOG_F(ERROR, "payout to unknown address %s", address.c_str());
    return;
  }

  {
    PayoutDbRecord record;
    record.userId = address;
    record.time = time(0);
    record.value = value;
    record.transactionId = transactionId;
    _payoutDb.put(record);
  }

  UserBalanceRecord &balance = It->second;
  balance.Requested -= (value+fee);
  if (balance.Requested < 0) {
    balance.Balance += balance.Requested;
    balance.Requested = 0;
  }

  balance.Paid += value;
  _balanceDb.put(balance);
}

bool AccountingDb::manualPayout(const std::string &user)
{
  auto It = _balanceMap.find(user);
  if (It != _balanceMap.end()) {
    auto &B = It->second;
    if (B.Balance >= ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE && B.Balance >= _cfg.MinimalAllowedPayout) {
      requestPayout(user, 0, true);
      return true;
    }
  }

  return false;
}
