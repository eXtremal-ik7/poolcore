#include "poolcore/accounting.h"
#include "poolcommon/coroutineJoin.h"
#include "poolcommon/debug.h"
#include "poolcommon/mergeSorted.h"
#include "poolcommon/utils.h"
#include "poolcore/priceFetcher.h"
#include "loguru.hpp"
#include <math.h>

static const UInt<384> ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE = fromRational(10000u);

void AccountingDb::CPPLNSPayoutAcc::merge(const CPPLNSPayout &record, unsigned fractionalPartSize)
{
  TotalCoin += record.PayoutValue;

  UInt<384> btcValue = record.PayoutValue;
  btcValue.mulfp(record.RateToBTC * std::pow(10.0, 8 - static_cast<int>(fractionalPartSize)));
  TotalBTC += btcValue;

  UInt<384> usdValue = record.PayoutValue;
  usdValue.mulfp(record.RateToBTC * record.RateBTCToUSD / std::pow(10.0, fractionalPartSize));
  TotalUSD += usdValue;

  TotalIncomingWork += record.AcceptedWork;
  PrimePOWTarget = std::min(PrimePOWTarget, record.PrimePOWTarget);
}

void AccountingDb::CPPLNSPayoutAcc::mergeScaled(const CPPLNSPayout &record, double coeff, unsigned fractionalPartSize)
{
  UInt<384> payoutValue = record.PayoutValue;
  payoutValue.mulfp(coeff);
  TotalCoin += payoutValue;

  UInt<384> btcValue = payoutValue;
  btcValue.mulfp(record.RateToBTC * std::pow(10.0, 8 - static_cast<int>(fractionalPartSize)));
  TotalBTC += btcValue;

  UInt<384> usdValue = payoutValue;
  usdValue.mulfp(record.RateToBTC * record.RateBTCToUSD / std::pow(10.0, fractionalPartSize));
  TotalUSD += usdValue;

  UInt<256> w = record.AcceptedWork;
  w.mulfp(coeff);
  TotalIncomingWork += w;

  PrimePOWTarget = std::min(PrimePOWTarget, record.PrimePOWTarget);
}

void AccountingDb::printRecentStatistic()
{
  if (State_.RecentStats.empty()) {
    LOG_F(INFO, "[%s] Recent statistic: empty", CoinInfo_.Name.c_str());
    return;
  }

  LOG_F(INFO, "[%s] Recent statistic:", CoinInfo_.Name.c_str());
  for (const auto &user: State_.RecentStats) {
    std::string line = user.UserId;
    line.append(": ");
    bool firstIter = true;
    for (const auto &stat: user.Recent) {
      if (!firstIter)
        line.append(", ");
      line.append(stat.SharesWork.getDecimal());
      firstIter = false;
    }

    LOG_F(INFO, " * %s", line.c_str());
  }
}

AccountingDb::CPersistentState::CPersistentState(const std::filesystem::path &dbPath)
  : Db(dbPath)
{
}

bool AccountingDb::CPersistentState::load()
{
  RecentStats.clear();
  CurrentScores.clear();

  std::unique_ptr<rocksdbBase::IteratorType> It(Db.iterator());
  It->seekFirst();

  while (It->valid()) {
    RawData key = It->key();
    RawData value = It->value();
    std::string keyStr(reinterpret_cast<const char*>(key.data), key.size);

    xmstream stream(value.data, value.size);
    stream.seekSet(0);

    if (keyStr == "lastmsgid") {
      DbIo<uint64_t>::unserialize(stream, SavedShareId);
    } else if (keyStr == "recentstats") {
      DbIo<decltype(RecentStats)>::unserialize(stream, RecentStats);
    } else if (keyStr == "currentscores") {
      DbIo<decltype(CurrentScores)>::unserialize(stream, CurrentScores);
    } else if (keyStr == "currentroundstart") {
      DbIo<Timestamp>::unserialize(stream, CurrentRoundStartTime);
    } else if (keyStr == "payoutqueue") {
      while (stream.remaining()) {
        PayoutDbRecord element;
        if (!element.deserializeValue(stream))
          break;
        PayoutQueue.push_back(element);
        KnownTransactions.insert(element.TransactionId);
      }
    }

    It->next();
  }

  // Fresh DB has no currentroundstart key; use current time as round start
  if (CurrentRoundStartTime == Timestamp())
    CurrentRoundStartTime = Timestamp::now();

  if (SavedShareId != 0) {
    LOG_F(INFO, "AccountingDb: loaded state from db, SavedShareId=%" PRIu64 "", SavedShareId);
    return true;
  }
  return false;
}

void AccountingDb::CPersistentState::flushCurrentScores(uint64_t lastKnownShareId)
{
  SavedShareId = lastKnownShareId;
  rocksdbBase::CBatch batch = Db.batch("default");

  // Save lastKnownShareId
  {
    xmstream stream;
    DbIo<uint64_t>::serialize(stream, lastKnownShareId);
    std::string key = "lastmsgid";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }

  // Save CurrentScores
  {
    xmstream stream;
    DbIo<decltype(CurrentScores)>::serialize(stream, CurrentScores);
    std::string key = "currentscores";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }

  Db.writeBatch(batch);
}

void AccountingDb::CPersistentState::flushBlockFoundState(uint64_t lastKnownShareId)
{
  SavedShareId = lastKnownShareId;
  rocksdbBase::CBatch batch = Db.batch("default");

  // Save lastKnownShareId
  {
    xmstream stream;
    DbIo<uint64_t>::serialize(stream, lastKnownShareId);
    std::string key = "lastmsgid";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }

  // Save RecentStats
  {
    xmstream stream;
    DbIo<decltype(RecentStats)>::serialize(stream, RecentStats);
    std::string key = "recentstats";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }

  // Save CurrentRoundStartTime
  {
    xmstream stream;
    DbIo<Timestamp>::serialize(stream, CurrentRoundStartTime);
    std::string key = "currentroundstart";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }

  // Delete CurrentScores (it's cleared after block found)
  {
    std::string key = "currentscores";
    batch.deleteRow(key.data(), key.size());
  }

  Db.writeBatch(batch);
}

void AccountingDb::CPersistentState::flushPayoutQueue()
{
  xmstream stream;
  for (auto &p: PayoutQueue)
    p.serializeValue(stream);

  std::string key = "payoutqueue";
  Db.put("default", key.data(), key.size(), stream.data(), stream.sizeOf());
}

void AccountingDb::flushUserStats(Timestamp timeLabel)
{
  UserStatsAcc_.flush(timeLabel, LastKnownShareId_, _cfg.dbPath, nullptr);
}

AccountingDb::AccountingDb(asyncBase *base,
                           const PoolBackendConfig &config,
                           const CCoinInfo &coinInfo,
                           UserManager &userMgr,
                           CNetworkClientDispatcher &clientDispatcher,
                           CPriceFetcher &priceFetcher) :
  Base_(base),
  _cfg(config),
  CoinInfo_(coinInfo),
  UserManager_(userMgr),
  ClientDispatcher_(clientDispatcher),
  PriceFetcher_(priceFetcher),
  State_(config.dbPath / "accounting.state"),
  _roundsDb(config.dbPath / "rounds.3"),
  _balanceDb(config.dbPath / "balance.2"),
  _foundBlocksDb(config.dbPath / "foundBlocks.2"),
  _poolBalanceDb(config.dbPath / "poolBalance.2"),
  _payoutDb(config.dbPath / "payouts.2"),
  PPLNSPayoutsDb(config.dbPath / "pplns.payouts.2"),
  ShareLog_(config.dbPath / "accounting.worklog", coinInfo.Name, config.ShareLogFileSizeLimit),
  UserStatsAcc_("accounting.userstats", config.StatisticUserGridInterval, config.AccountingPPLNSWindow * 2),
  TaskHandler_(this, base)
{
  FlushTimerEvent_ = newUserEvent(base, 1, nullptr, nullptr);
  ShareLogFlushEvent_ = newUserEvent(base, 1, nullptr, nullptr);

  State_.load();
  UserStatsAcc_.load(_cfg.dbPath, coinInfo.Name);
  UserStatsFlushEvent_ = newUserEvent(base, 1, nullptr, nullptr);
  LastKnownShareId_ = std::max(State_.SavedShareId, UserStatsAcc_.savedShareId());
  LOG_F(INFO, "loaded %u payouts from db", static_cast<unsigned>(State_.PayoutQueue.size()));

  {
    std::unique_ptr<rocksdbBase::IteratorType> It(_roundsDb.iterator());
    It->seekFirst();
    for (; It->valid(); It->next()) {
      MiningRound *R = new MiningRound;
      RawData data = It->value();
      if (R->deserializeValue(data.data, data.size)) {
        _allRounds.emplace_back(R);
        if (!R->Payouts.empty())
          UnpayedRounds_.insert(R);
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

  Timestamp initTime = Timestamp::now();
  ShareLog_.replay([this](uint64_t messageId, const std::vector<CUserWorkSummary> &scores) {
    replayUserWorkSummary(messageId, scores);
  });

  UserStatsAcc_.setAccumulationBegin(initTime);
  printRecentStatistic();
  if (!State_.CurrentScores.empty()) {
    LOG_F(INFO, "[%s] current scores:", CoinInfo_.Name.c_str());
    for (const auto &It: State_.CurrentScores)
      LOG_F(INFO, " * %s: %s", It.first.c_str(), It.second.getDecimal().c_str());
  } else {
    LOG_F(INFO, "[%s] current scores is empty", CoinInfo_.Name.c_str());
  }
  if (isDebugStatistic()) {
    LOG_F(1, "initializationFinish: timeLabel: %" PRIi64 "", initTime.toUnixTime());
    LOG_F(1, "%s: replayed %" PRIu64 " shares from %" PRIu64 " to %" PRIu64 "", coinInfo.Name.c_str(), Dbg_.Count, Dbg_.MinShareId, Dbg_.MaxShareId);
  }

  ShareLog_.startLogging(lastKnownShareId() + 1);
}

void AccountingDb::shareLogFlushHandler()
{
  for (;;) {
    ioSleep(ShareLogFlushEvent_, std::chrono::microseconds(_cfg.ShareLogFlushInterval).count());
    ShareLog_.flush();
    if (ShutdownRequested_)
      break;
    ShareLog_.cleanupOldFiles(lastAggregatedShareId());
  }
}

void AccountingDb::start()
{
  TaskHandler_.start();
  coroutineCall(coroutineNewWithCb([](void *arg) { static_cast<AccountingDb*>(arg)->shareLogFlushHandler(); }, this, 0x100000, coroutineFinishCb, &ShareLogFlushFinished_));

  coroutineCall(coroutineNewWithCb([](void *arg) {
    AccountingDb *db = static_cast<AccountingDb*>(arg);
    for (;;) {
      ioSleep(db->FlushTimerEvent_, std::chrono::microseconds(std::chrono::minutes(1)).count());
      db->State_.flushCurrentScores(db->LastKnownShareId_);
      if (db->ShutdownRequested_)
        break;
    }
  }, this, 0x20000, coroutineFinishCb, &FlushFinished_));

  coroutineCall(coroutineNewWithCb([](void *arg) {
    AccountingDb *db = static_cast<AccountingDb*>(arg);
    for (;;) {
      ioSleep(db->UserStatsFlushEvent_, std::chrono::microseconds(db->_cfg.StatisticUserFlushInterval).count());
      db->flushUserStats(Timestamp::now());
      if (db->ShutdownRequested_)
        break;
    }
  }, this, 0x20000, coroutineFinishCb, &UserStatsFlushFinished_));
}

void AccountingDb::stop()
{
  TaskHandler_.stop(CoinInfo_.Name.c_str(), "accounting: task handler");
  ShutdownRequested_ = true;
  userEventActivate(FlushTimerEvent_);
  userEventActivate(UserStatsFlushEvent_);
  userEventActivate(ShareLogFlushEvent_);
  coroutineJoin(CoinInfo_.Name.c_str(), "accounting: flush thread", &FlushFinished_);
  coroutineJoin(CoinInfo_.Name.c_str(), "accounting: user stats flush", &UserStatsFlushFinished_);
  coroutineJoin(CoinInfo_.Name.c_str(), "accounting: share log flush", &ShareLogFlushFinished_);
  ShareLog_.flush();
}


void AccountingDb::cleanupRounds()
{
  Timestamp timeLabel = Timestamp::now() - std::chrono::seconds(_cfg.KeepRoundTime);
  auto I = _allRounds.begin();
  while (I != _allRounds.end()) {
    MiningRound *round = I->get();
    if (round->EndTime >= timeLabel || UnpayedRounds_.count(round))
      break;
    _roundsDb.deleteRow(*round);
    ++I;
  }

  if (I != _allRounds.begin()) {
    LOG_F(INFO, "delete %u old rounds", (unsigned)std::distance(_allRounds.begin(), I));
    _allRounds.erase(_allRounds.begin(), I);
  }
}

bool AccountingDb::hasUnknownReward()
{
  return CoinInfo_.HasDagFile;
}

void AccountingDb::calculatePayments(MiningRound *R, const UInt<384> &generatedCoins)
{
  R->AvailableCoins = generatedCoins;
  R->Payouts.clear();

  if (R->TotalShareValue.isZero()) {
    LOG_F(ERROR, "Block found but TotalShareValue is zero, skipping payouts");
    return;
  }

  UInt<384> totalPayout = UInt<384>::zero();
  std::vector<CUserPayout> payouts;
  std::map<std::string, UInt<384>> feePayouts;
  std::unordered_map<std::string, UserManager::UserFeeConfig> feePlans;
  UInt<384> minShareCost = R->AvailableCoins / R->TotalShareValue;
  for (const auto &record: R->UserShares) {
    // Calculate payout
    UInt<384> payoutValue = minShareCost * record.ShareValue;
    totalPayout += payoutValue;

    // get fee plan for user
    std::string feePlanId = UserManager_.getFeePlanId(record.UserId);
    auto It = feePlans.find(feePlanId);
    if (It == feePlans.end())
      It = feePlans.insert(It, std::make_pair(feePlanId, UserManager_.getFeeRecord(feePlanId, CoinInfo_.Name)));

    UserManager::UserFeeConfig &feeRecord = It->second;

    UInt<384> feeValuesSum = UInt<384>::zero();
    std::vector<UInt<384>> feeValues;
    for (const auto &poolFeeRecord: feeRecord) {
      UInt<384> value = payoutValue;
        // mulfp uses 53-bit mantissa; ±1 satoshi possible for amounts > 2^53
        value.mulfp(poolFeeRecord.Percentage / 100.0);
      feeValues.push_back(value);
      feeValuesSum += value;
    }

    std::string debugString;
    if (feeValuesSum <= payoutValue) {
      for (size_t i = 0, ie = feeRecord.size(); i != ie; ++i) {
        debugString.append(feeRecord[i].UserId);
        debugString.push_back('(');
        debugString.append(FormatMoney(feeValues[i], CoinInfo_.FractionalPartSize));
        debugString.append(") ");
        feePayouts[feeRecord[i].UserId] += feeValues[i];
      }

      payoutValue -= feeValuesSum;
    } else {
      feeValuesSum = UInt<384>::zero();
      feeValues.clear();
      debugString = "NONE";
      LOG_F(ERROR, "   * user %s: fee over 100%% can't be applied", record.UserId.c_str());
    }

    payouts.emplace_back(record.UserId, payoutValue, payoutValue, record.IncomingWork);
    LOG_F(INFO, " * %s %s -> %sremaining %s", record.UserId.c_str(), FormatMoney(payoutValue+feeValuesSum, CoinInfo_.FractionalPartSize).c_str(), debugString.c_str(), FormatMoney(payoutValue, CoinInfo_.FractionalPartSize).c_str());
  }

  mergeSorted(payouts.begin(), payouts.end(), feePayouts.begin(), feePayouts.end(),
    [](const CUserPayout &l, const std::pair<std::string, UInt<384>> &r) { return l.UserId < r.first; },
    [](const std::pair<std::string, UInt<384>> &l, const CUserPayout &r) { return l.first < r.UserId; },
    [R](const CUserPayout &record) {
      if (!record.Value.isZero())
        R->Payouts.emplace_back(record);
    }, [R](const std::pair<std::string, UInt<384>> &fee) {
      if (!fee.second.isZero())
        R->Payouts.emplace_back(fee.first, fee.second, UInt<384>::zero(), UInt<256>::zero());
    }, [R](const CUserPayout &record, const std::pair<std::string, UInt<384>> &fee) {
    if (!(record.Value + fee.second).isZero())
        R->Payouts.emplace_back(record.UserId, record.Value + fee.second, record.Value, record.AcceptedWork);
    });

  // Correct payouts for use all available coins
  if (!R->Payouts.empty()) {
    UInt<384> diff;
    UInt<384> div;
    bool needSubtract = totalPayout > generatedCoins;

    if (needSubtract) {
      diff = totalPayout - generatedCoins;
    } else {
      diff = generatedCoins - totalPayout;
    }
    uint64_t mod = diff.divmod64(R->Payouts.size(), &div);

    totalPayout = UInt<384>();
    uint64_t i = 0;
    for (auto I = R->Payouts.begin(), IE = R->Payouts.end(); I != IE; ++I, ++i) {
      if (needSubtract) {
        I->Value -= div;
        if (i < mod)
          I->Value -= 1u;
      } else {
        I->Value += div;
        if (i < mod)
          I->Value += 1u;
      }
      totalPayout += I->Value;
      LOG_F(INFO, "   * %s: payout: %s", I->UserId.c_str(), FormatMoney(I->Value, CoinInfo_.FractionalPartSize).c_str());
    }

    LOG_F(INFO, " * total payout (after correct): %s", FormatMoney(totalPayout, CoinInfo_.FractionalPartSize).c_str());
  }
}

void AccountingDb::onUserWorkSummary(const std::vector<CUserWorkSummary> &scores)
{
  uint64_t messageId = ShareLog_.addShare(scores);
  for (const auto &score : scores) {
    State_.CurrentScores[score.UserId] += score.AcceptedWork;
    UserStatsAcc_.addBaseWork(score.UserId, "", score.SharesNum, score.AcceptedWork, score.Time);
  }
  LastKnownShareId_ = std::max(LastKnownShareId_, messageId);
}

void AccountingDb::onBlockFound(const CBlockFoundData &block)
{
  UInt<256> accumulatedWork = UInt<256>::zero();
  for (const auto &score: State_.CurrentScores)
    accumulatedWork += score.second;

  {
    // save to database
    FoundBlockRecord blk;
    blk.Height = block.Height;
    blk.Hash = block.Hash;
    blk.Time = block.Time;
    blk.AvailableCoins = block.GeneratedCoins;
    blk.FoundBy = block.UserId;
    blk.ExpectedWork = block.ExpectedWork;
    blk.AccumulatedWork = accumulatedWork;
    if (hasUnknownReward())
      blk.PublicHash = "?";
    _foundBlocksDb.put(blk);
  }

  MiningRound *R = new MiningRound;

  LOG_F(INFO, " * block height: %u, hash: %s, value: %s", (unsigned)block.Height, block.Hash.c_str(), FormatMoney(block.GeneratedCoins, CoinInfo_.FractionalPartSize).c_str());

  R->Height = block.Height;
  R->BlockHash = block.Hash;
  R->EndTime = block.Time;
  R->FoundBy = block.UserId;
  R->ExpectedWork = block.ExpectedWork;
  R->AccumulatedWork = accumulatedWork;
  R->TotalShareValue = UInt<256>::zero();
  R->PrimePOWTarget = block.PrimePOWTarget;

  R->StartTime = State_.CurrentRoundStartTime;

  // Merge shares for current block with older shares (PPLNS)
  // RecentStats is a snapshot from the previous block-found event; CurrentScores covers
  // all work since then (replayed from ShareLog after restart). Together they always span
  // the full PPLNS window — no re-export from UserStatsAcc_ is needed here.
  {
    Timestamp acceptSharesTime = block.Time - _cfg.AccountingPPLNSWindow;
    mergeSorted(State_.RecentStats.begin(), State_.RecentStats.end(), State_.CurrentScores.begin(), State_.CurrentScores.end(),
      [](const CStatsExportData &stats, const std::pair<std::string, UInt<256>> &scores) { return stats.UserId < scores.first; },
      [](const std::pair<std::string, UInt<256>> &scores, const CStatsExportData &stats) { return scores.first < stats.UserId; },
      [&](const CStatsExportData &stats) {
        // User disconnected recently, no new shares
        UInt<256> shareValue = stats.recentShareValue(acceptSharesTime);
        if (shareValue.nonZero()) {
          R->UserShares.emplace_back(stats.UserId, shareValue, UInt<256>::zero());
        }
      }, [&](const std::pair<std::string, UInt<256>> &scores) {
        // User joined recently, no extra shares in statistic
        R->UserShares.emplace_back(scores.first, scores.second, scores.second);
      }, [&](const CStatsExportData &stats, const std::pair<std::string, UInt<256>> &scores) {
        // Need merge new shares and recent share statistics
        R->UserShares.emplace_back(stats.UserId, scores.second + stats.recentShareValue(acceptSharesTime), scores.second);
      });
  }

  // Calculate total share value
  for (const auto &element: R->UserShares)
    R->TotalShareValue += element.ShareValue;

  // Calculate payments
  if (!hasUnknownReward())
    calculatePayments(R, block.GeneratedCoins);

  // NOTE: crash between _roundsDb.put and flushBlockFoundState will leave
  // data inconsistent — round is persisted but State_ (CurrentScores,
  // RecentStats, CurrentRoundStartTime) is not updated. On restart the
  // old CurrentScores will be re-accumulated into the next round.

  // store round to DB and clear shares map
  _allRounds.emplace_back(R);
  _roundsDb.put(*R);
  UnpayedRounds_.insert(R);

  // Query statistics
  UserStatsAcc_.exportRecentStats(_cfg.AccountingPPLNSWindow, State_.RecentStats);
  printRecentStatistic();

  // Reset aggregated data
  State_.CurrentScores.clear();
  State_.CurrentRoundStartTime = R->EndTime;

  // Save state to db
  State_.flushBlockFoundState(LastKnownShareId_);
}

void AccountingDb::replayUserWorkSummary(uint64_t messageId, const std::vector<CUserWorkSummary> &scores)
{
  for (const auto &score : scores) {
    if (messageId > State_.SavedShareId)
      State_.CurrentScores[score.UserId] += score.AcceptedWork;
    if (messageId > UserStatsAcc_.savedShareId())
      UserStatsAcc_.addBaseWork(score.UserId, "", score.SharesNum, score.AcceptedWork, score.Time);
  }

  LastKnownShareId_ = std::max(LastKnownShareId_, messageId);
  if (isDebugAccounting()) {
    Dbg_.MinShareId = std::min(Dbg_.MinShareId, messageId);
    Dbg_.MaxShareId = std::max(Dbg_.MaxShareId, messageId);
    if (messageId > State_.SavedShareId)
      Dbg_.Count++;
  }
}

void AccountingDb::processRoundConfirmation(MiningRound *R, int64_t confirmations, const std::string &hash, bool *roundUpdated)
{
  *roundUpdated = false;
  if (confirmations == -1) {
    LOG_F(INFO, "block %" PRIu64 "/%s marked as orphan, can't do any payout", R->Height, hash.c_str());
    R->Payouts.clear();
    UnpayedRounds_.erase(R);
    *roundUpdated = true;
  } else if (confirmations >= _cfg.RequiredConfirmations) {
    LOG_F(INFO, "Make payout for block %" PRIu64 "/%s", R->Height, R->BlockHash.c_str());
    for (auto I = R->Payouts.begin(), IE = R->Payouts.end(); I != IE; ++I) {
      requestPayout(I->UserId, I->Value);

      {
        CPPLNSPayout record;
        record.Login = I->UserId;
        record.RoundStartTime = R->StartTime;
        record.BlockHash = R->BlockHash;
        record.BlockHeight = R->Height;
        record.RoundEndTime = R->EndTime;
        record.PayoutValue = I->Value;
        record.PayoutValueWithoutFee = I->ValueWithoutFee;
        record.AcceptedWork = I->AcceptedWork;
        record.PrimePOWTarget = R->PrimePOWTarget;
        record.RateToBTC = PriceFetcher_.getPrice(CoinInfo_.Name);
        record.RateBTCToUSD = PriceFetcher_.getBtcUsd();
        PPLNSPayoutsDb.put(record);
      }
    }

    R->Payouts.clear();
    UnpayedRounds_.erase(R);
    *roundUpdated = true;
  }
}

void AccountingDb::checkBlockConfirmations()
{
  if (UnpayedRounds_.empty())
    return;

  LOG_F(INFO, "Checking %zu blocks for confirmations...", UnpayedRounds_.size());
  std::vector<MiningRound*> rounds(UnpayedRounds_.begin(), UnpayedRounds_.end());

  std::vector<CNetworkClient::GetBlockConfirmationsQuery> confirmationsQuery(rounds.size());
  for (size_t i = 0, ie = rounds.size(); i != ie; ++i) {
    confirmationsQuery[i].Hash = rounds[i]->BlockHash;
    confirmationsQuery[i].Height = rounds[i]->Height;
  }

  if (!ClientDispatcher_.ioGetBlockConfirmations(Base_, _cfg.RequiredConfirmations, confirmationsQuery)) {
    LOG_F(ERROR, "ioGetBlockConfirmations api call failed");
    return;
  }

  for (size_t i = 0; i < confirmationsQuery.size(); i++) {
    bool roundUpdated = false;
    processRoundConfirmation(rounds[i], confirmationsQuery[i].Confirmations, confirmationsQuery[i].Hash, &roundUpdated);
    if (roundUpdated)
      _roundsDb.put(*rounds[i]);
  }

  State_.flushPayoutQueue();
}

void AccountingDb::checkBlockExtraInfo()
{
  if (UnpayedRounds_.empty())
    return;

  LOG_F(INFO, "Checking %zu blocks for extra info...", UnpayedRounds_.size());
  std::vector<MiningRound*> unpayedRounds(UnpayedRounds_.begin(), UnpayedRounds_.end());

  std::vector<CNetworkClient::GetBlockExtraInfoQuery> confirmationsQuery;
  for (const auto &round: unpayedRounds)
    confirmationsQuery.emplace_back(round->BlockHash, round->Height, round->TxFee, round->AvailableCoins);

  if (!ClientDispatcher_.ioGetBlockExtraInfo(Base_, _cfg.RequiredConfirmations, confirmationsQuery)) {
    LOG_F(ERROR, "ioGetBlockExtraInfo api call failed");
    return;
  }

  for (size_t i = 0; i < confirmationsQuery.size(); i++) {
    MiningRound *R = unpayedRounds[i];

    bool rewardChanged = R->AvailableCoins != confirmationsQuery[i].BlockReward;
    if (rewardChanged) {
      // Update found block database
      FoundBlockRecord blk;
      blk.Height = R->Height;
      blk.Hash = R->BlockHash;
      blk.Time = R->EndTime;
      blk.AvailableCoins = confirmationsQuery[i].BlockReward;
      blk.FoundBy = R->FoundBy;
      blk.ExpectedWork = R->ExpectedWork;
      blk.AccumulatedWork = R->AccumulatedWork;
      blk.PublicHash = confirmationsQuery[i].PublicHash;
      _foundBlocksDb.put(blk);

      // Update payment info
      R->TxFee = confirmationsQuery[i].TxFee;
      calculatePayments(R, confirmationsQuery[i].BlockReward);
    }

    bool roundUpdated = false;
    processRoundConfirmation(R, confirmationsQuery[i].Confirmations, confirmationsQuery[i].Hash, &roundUpdated);
    if (rewardChanged || roundUpdated)
      _roundsDb.put(*R);
  }

  State_.flushPayoutQueue();
}

void AccountingDb::buildTransaction(PayoutDbRecord &payout, unsigned index, std::string &recipient, bool *needSkipPayout)
{
  *needSkipPayout = false;
  if (payout.Value < _cfg.MinimalAllowedPayout) {
    LOG_F(INFO,
          "[%u] Accounting: ignore this payout to %s, value is %s, minimal is %s",
          index,
          payout.UserId.c_str(),
          FormatMoney(payout.Value, CoinInfo_.FractionalPartSize).c_str(),
          FormatMoney(_cfg.MinimalAllowedPayout, CoinInfo_.FractionalPartSize).c_str());
    *needSkipPayout = true;
    return;
  }

  // Get address for payment
  UserSettingsRecord settings;
  bool hasSettings = UserManager_.getUserCoinSettings(payout.UserId, CoinInfo_.Name, settings);
  if (!hasSettings || settings.Address.empty()) {
    LOG_F(WARNING, "user %s did not setup payout address, ignoring", payout.UserId.c_str());
    *needSkipPayout = true;
    return;
  }

  recipient = settings.Address;
  if (!CoinInfo_.checkAddress(settings.Address, CoinInfo_.PayoutAddressType)) {
    LOG_F(ERROR, "Invalid payment address %s for %s", settings.Address.c_str(), payout.UserId.c_str());
    *needSkipPayout = true;
    return;
  }

  // Build transaction
  // For bitcoin-based API it's sequential call of createrawtransaction, fundrawtransaction and signrawtransaction
  CNetworkClient::BuildTransactionResult transaction;
  CNetworkClient::EOperationStatus status =
    ClientDispatcher_.ioBuildTransaction(Base_, settings.Address.c_str(), _cfg.MiningAddresses.get().MiningAddress, payout.Value, transaction);
  if (status == CNetworkClient::EStatusOk) {
    // Nothing to do
  } else if (status == CNetworkClient::EStatusInsufficientFunds) {
    LOG_F(INFO, "No money left to pay");
    return;
  } else {
    LOG_F(ERROR, "Payment %s to %s failed with error \"%s\"", FormatMoney(payout.Value, CoinInfo_.FractionalPartSize).c_str(), settings.Address.c_str(), transaction.Error.c_str());
    return;
  }

  // int64_t delta = payout.Value - (transaction.Value + transaction.Fee);
  UInt<384> transactionTotalValue = transaction.Value + transaction.Fee;

  if (payout.Value > transactionTotalValue) {
    // Correct payout value and request balance
    UInt<384> delta = payout.Value - transactionTotalValue;
    payout.Value -= delta;

    // Update user balance
    auto It = _balanceMap.find(payout.UserId);
    if (It == _balanceMap.end()) {
      LOG_F(ERROR, "payout to unknown address %s", payout.UserId.c_str());
      return;
    }

    LOG_F(INFO, "   * correct requested balance for %s by %s", payout.UserId.c_str(), FormatMoney(delta, CoinInfo_.FractionalPartSize).c_str());
    UserBalanceRecord &balance = It->second;
    balance.Requested -= delta;
    _balanceDb.put(balance);
  } else if (payout.Value < transactionTotalValue) {
    LOG_F(ERROR, "Payment %s to %s failed: too big transaction amount", FormatMoney(payout.Value, CoinInfo_.FractionalPartSize).c_str(), settings.Address.c_str());
    return;
  }

  // Save transaction to database
  if (!State_.KnownTransactions.insert(transaction.TxId).second) {
    LOG_F(ERROR, "Node generated duplicate for transaction %s !!!", transaction.TxId.c_str());
    return;
  }

  payout.TransactionData = transaction.TxData;
  payout.TransactionId = transaction.TxId;
  payout.Time = time(nullptr);
  payout.Status = PayoutDbRecord::ETxCreated;
  _payoutDb.put(payout);
}

bool AccountingDb::sendTransaction(PayoutDbRecord &payout)
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
    _payoutDb.put(payout);

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
  _payoutDb.put(payout);
  return true;
}

bool AccountingDb::checkTxConfirmations(PayoutDbRecord &payout)
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
    _payoutDb.put(payout);

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
  if (confirmations >= _cfg.RequiredConfirmations) {
    payout.Status = PayoutDbRecord::ETxConfirmed;
    _payoutDb.put(payout);

    // Update user balance
    auto It = _balanceMap.find(payout.UserId);
    if (It == _balanceMap.end()) {
      LOG_F(ERROR, "payout to unknown address %s", payout.UserId.c_str());
      return false;
    }

    UserBalanceRecord &balance = It->second;
    // Balance can become negative (unsigned underflow) if fees exceed expectations.
    // This is handled: requestPayout/manualPayoutImpl check isNegative() before queuing new payouts.
    balance.Balance -= (payout.Value + payout.TxFee);
    balance.Requested -= payout.Value;
    balance.Paid += payout.Value;
    _balanceDb.put(balance);
    return true;
  }

  return false;
}

void AccountingDb::makePayout()
{
  if (!State_.PayoutQueue.empty()) {
    LOG_F(INFO, "Accounting: checking %u payout requests...", (unsigned)State_.PayoutQueue.size());

    // Merge small payouts and payouts to invalid address
    // TODO: merge small payouts with normal also
    {
      std::map<std::string, UInt<384>> payoutAccMap;
      for (auto I = State_.PayoutQueue.begin(), IE = State_.PayoutQueue.end(); I != IE;) {
        if (I->Status != PayoutDbRecord::EInitialized) {
          ++I;
          continue;
        }

        if (I->Value < _cfg.MinimalAllowedPayout) {
          payoutAccMap[I->UserId] += I->Value;
          LOG_F(INFO,
                "Accounting: merge payout %s for %s (total already %s)",
                FormatMoney(I->Value, CoinInfo_.FractionalPartSize).c_str(),
                I->UserId.c_str(),
                FormatMoney(payoutAccMap[I->UserId], CoinInfo_.FractionalPartSize).c_str());
          State_.PayoutQueue.erase(I++);
        } else {
          ++I;
        }
      }

      for (const auto &I: payoutAccMap)
        State_.PayoutQueue.push_back(PayoutDbRecord(I.first, I.second));
    }

    unsigned index = 0;
    for (auto &payout: State_.PayoutQueue) {
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
    for (auto I = State_.PayoutQueue.begin(), IE = State_.PayoutQueue.end(); I != IE;) {
      if (I->Status == PayoutDbRecord::ETxConfirmed) {
        State_.KnownTransactions.erase(I->TransactionId);
        State_.PayoutQueue.erase(I++);
      } else {
        ++I;
      }
    }

    State_.flushPayoutQueue();
  }

  if (!_cfg.poolZAddr.empty() && !_cfg.poolTAddr.empty()) {
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
        CNetworkClient::EOperationStatus status = ClientDispatcher_.ioZSendMoney(Base_, out.first, _cfg.poolZAddr, out.second, "", 1, UInt<384>::zero(), zsendResult);
        if (status == CNetworkClient::EStatusOk && !zsendResult.AsyncOperationId.empty()) {
          LOG_F(INFO,
                " * moving %s coins from %s to %s started (%s)",
                FormatMoney(out.second, CoinInfo_.FractionalPartSize).c_str(),
                out.first.c_str(),
                _cfg.poolZAddr.c_str(),
                zsendResult.AsyncOperationId.c_str());
        } else {
          LOG_F(INFO,
                " * async operation start error %s: source=%s, destination=%s, amount=%s",
                !zsendResult.Error.empty() ? zsendResult.Error.c_str() : "<unknown error>",
                out.first.c_str(),
                _cfg.poolZAddr.c_str(),
                FormatMoney(out.second, CoinInfo_.FractionalPartSize).c_str());
        }
      }
    }

    // move Z-Addr to T-Addr
    UInt<384> zbalance;
    if (ClientDispatcher_.ioZGetBalance(Base_, _cfg.poolZAddr, &zbalance) == CNetworkClient::EStatusOk && zbalance.nonZero()) {
      LOG_F(INFO, "Accounting: move %s coins to transparent address", FormatMoney(zbalance, CoinInfo_.FractionalPartSize).c_str());
      CNetworkClient::ZSendMoneyResult zsendResult;
      if (ClientDispatcher_.ioZSendMoney(Base_, _cfg.poolZAddr, _cfg.poolTAddr, zbalance, "", 1, UInt<384>::zero(), zsendResult) == CNetworkClient::EStatusOk) {
        LOG_F(INFO,
              "moving %s coins from %s to %s started (%s)",
              FormatMoney(zbalance, CoinInfo_.FractionalPartSize).c_str(),
              _cfg.poolZAddr.c_str(),
              _cfg.poolTAddr.c_str(),
              !zsendResult.AsyncOperationId.empty() ? zsendResult.AsyncOperationId.c_str() : "<none>");
      }
    }
  }

  // Check consistency
  std::unordered_map<std::string, UInt<384>> enqueued;
  for (const auto &payout: State_.PayoutQueue)
    enqueued[payout.UserId] += payout.Value;

  bool inconsistent = false;
  for (auto &userIt: _balanceMap) {
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

void AccountingDb::checkBalance()
{
  UInt<384> balance = UInt<384>::zero();
  UInt<384> requestedInBalance = UInt<384>::zero();
  UInt<384> requestedInQueue = UInt<384>::zero();
  UInt<384> confirmationWait = UInt<384>::zero();
  UInt<384> immature = UInt<384>::zero();
  UInt<384> userBalance = UInt<384>::zero();
  UInt<384> queued = UInt<384>::zero();
  UInt<384> net = UInt<384>::zero();

  UInt<384> zbalance = UInt<384>::zero();
  if (!_cfg.poolZAddr.empty()) {
    if (ClientDispatcher_.ioZGetBalance(Base_, _cfg.poolZAddr, &zbalance) != CNetworkClient::EStatusOk) {
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
    userBalance += userIt.second.Balance;
    requestedInBalance += userIt.second.Requested;
  }
  for (auto &p: State_.PayoutQueue) {
    requestedInQueue += p.Value;
    if (p.Status == PayoutDbRecord::ETxSent)
      confirmationWait += p.Value + p.TxFee;
  }

  for (auto &roundIt: UnpayedRounds_) {
    for (auto &pIt: roundIt->Payouts)
      queued += pIt.Value;
  }
  net = balance + immature - userBalance - queued + confirmationWait;

  {
    PoolBalanceRecord pb;
    pb.Time = time(0);
    pb.Balance = balance;
    pb.Immature = immature;
    pb.Users = userBalance;
    pb.Queued = queued;
    pb.ConfirmationWait = confirmationWait;
    pb.Net = net;
    _poolBalanceDb.put(pb);
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
}

bool AccountingDb::requestPayout(const std::string &address, const UInt<384> &value, bool force)
{
  bool result = false;
  auto It = _balanceMap.find(address);
  if (It == _balanceMap.end())
    It = _balanceMap.insert(It, std::make_pair(address, UserBalanceRecord(address, _cfg.DefaultPayoutThreshold)));

  UserBalanceRecord &balance = It->second;
  balance.Balance += value;

  UserSettingsRecord settings;
  bool hasSettings = UserManager_.getUserCoinSettings(balance.Login, CoinInfo_.Name, settings);
  UInt<384> nonQueuedBalance = balance.Balance - balance.Requested;
  if (!nonQueuedBalance.isNegative() && hasSettings && (force || (settings.AutoPayout && nonQueuedBalance >= settings.MinimalPayout))) {
    State_.PayoutQueue.push_back(PayoutDbRecord(address, nonQueuedBalance));
    balance.Requested += nonQueuedBalance;
    result = true;
  }

  _balanceDb.put(balance);
  return result;
}

void AccountingDb::manualPayoutImpl(const std::string &user, DefaultCb callback)
{
  auto It = _balanceMap.find(user);
  if (It != _balanceMap.end()) {
    auto &B = It->second;
    UInt<384> nonQueuedBalance = B.Balance - B.Requested;
    // Check global minimum (dust protection); per-user MinimalPayout is bypassed for manual payouts
    if (!nonQueuedBalance.isNegative() && nonQueuedBalance >= _cfg.MinimalAllowedPayout) {
      bool result = requestPayout(user, UInt<384>::zero(), true);
      const char *status = result ? "ok" : "payout_error";
      if (result) {
        LOG_F(INFO, "Manual payout success for %s", user.c_str());
        State_.flushPayoutQueue();
      }
      callback(status);
      return;
    } else {
      callback("insufficient_balance");
      return;
    }
  } else {
    callback("no_balance");
    return;
  }
}

void AccountingDb::queryFoundBlocksImpl(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback)
{
  auto &db = getFoundBlocksDb();
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());
  if (heightFrom != -1) {
    FoundBlockRecord blk;
    blk.Height = heightFrom;
    blk.Hash = hashFrom;
    It->seek(blk);
    It->prev();
  } else {
    It->seekLast();
  }

  std::vector<CNetworkClient::GetBlockConfirmationsQuery> confirmationsQuery;
  std::vector<FoundBlockRecord> foundBlocks;
  for (unsigned i = 0; i < count && It->valid(); i++) {
    FoundBlockRecord dbBlock;
    RawData data = It->value();
    if (!dbBlock.deserializeValue(data.data, data.size))
      break;

    // Replace login with public name
    UserManager::Credentials credentials;
    if (UserManager_.getUserCredentials(dbBlock.FoundBy, credentials) && !credentials.Name.empty())
      dbBlock.FoundBy = credentials.Name;

    foundBlocks.push_back(dbBlock);
    confirmationsQuery.emplace_back(dbBlock.Hash, dbBlock.Height);
    It->prev();
  }

  // query confirmations
  if (count)
    ClientDispatcher_.ioGetBlockConfirmations(Base_, _cfg.RequiredConfirmations, confirmationsQuery);

  callback(foundBlocks, confirmationsQuery);
}

void AccountingDb::queryPPLNSPayoutsImpl(const std::string &login, int64_t timeFrom, const std::string &hashFrom, uint32_t count, QueryPPLNSPayoutsCallback callback)
{
  auto &db = getPPLNSPayoutsDb();
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());

  CPPLNSPayout valueRecord;
  xmstream resumeKey;
  auto validPredicate = [&login](const CPPLNSPayout &record) -> bool {
    return record.Login == login;
  };

  {
    CPPLNSPayout record;
    record.Login = login;
    record.RoundStartTime = Timestamp(std::numeric_limits<int64_t>::max());
    record.serializeKey(resumeKey);
  }

  {
    CPPLNSPayout keyRecord;
    keyRecord.Login = login;
    keyRecord.RoundStartTime = timeFrom == 0 ? Timestamp(std::numeric_limits<int64_t>::max()) : Timestamp::fromUnixTime(timeFrom);
    keyRecord.BlockHash = hashFrom;
    It->seekForPrev<CPPLNSPayout>(keyRecord, resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  std::vector<CPPLNSPayout> payouts;
  for (unsigned i = 0; i < count && It->valid(); i++) {
    payouts.emplace_back(valueRecord);
    It->prev<CPPLNSPayout>(resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  callback(payouts);
}

void AccountingDb::queryPPLNSPayoutsAccImpl(const std::string &login, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, QueryPPLNSAccCallback callback)
{
  std::vector<CPPLNSPayoutAcc> payoutAccs;

  // Maximum number of output cells to prevent excessive memory/CPU usage
  constexpr int64_t MaxOutputCells = 3200;

  // TODO: return error
  if (timeTo <= timeFrom ||
      groupByInterval <= 0 ||
      (timeTo - timeFrom) % groupByInterval != 0 ||
      (timeTo - timeFrom) / groupByInterval > MaxOutputCells) {
    callback(payoutAccs);
    return;
  }

  auto &db = getPPLNSPayoutsDb();
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());

  CPPLNSPayout valueRecord;
  xmstream resumeKey;
  auto validPredicate = [&login](const CPPLNSPayout &record) -> bool {
    return record.Login == login;
  };

  Timestamp gridStart = Timestamp::fromUnixTime(timeFrom);
  Timestamp gridEnd = Timestamp::fromUnixTime(timeTo);
  auto groupBy = std::chrono::seconds(groupByInterval);
  size_t count = (gridEnd - gridStart) / groupBy;

  payoutAccs.resize(count);
  for (size_t i = 0; i < count; i++)
    payoutAccs[i].IntervalEnd = timeFrom + groupByInterval * static_cast<int64_t>(i + 1);

  {
    CPPLNSPayout record;
    record.Login = login;
    record.RoundStartTime = Timestamp(std::numeric_limits<int64_t>::max());
    record.BlockHash.clear();
    record.serializeKey(resumeKey);
  }

  {
    CPPLNSPayout keyRecord;
    keyRecord.Login = login;
    keyRecord.RoundStartTime = gridEnd;
    keyRecord.BlockHash.clear();
    It->seekForPrev<CPPLNSPayout>(keyRecord, resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  // Rounds form a continuous chain (each RoundStartTime == previous RoundEndTime),
  // so RoundEndTime decreases monotonically — safe to break early.
  while (It->valid()) {
    if (valueRecord.RoundEndTime <= gridStart)
      break;

    Timestamp clampedBegin = std::max(valueRecord.RoundStartTime, gridStart);
    Timestamp clampedEnd = std::min(valueRecord.RoundEndTime, gridEnd);

    size_t firstIdx = static_cast<size_t>((clampedBegin - gridStart) / groupBy);
    size_t lastIdx = static_cast<size_t>((clampedEnd - gridStart + groupBy - std::chrono::milliseconds(1)) / groupBy);

    Timestamp cellBegin = gridStart + groupBy * static_cast<int64_t>(firstIdx);
    Timestamp cellEnd = cellBegin + groupBy;
    for (size_t i = firstIdx; i < lastIdx; i++) {
      double coeff = overlapFraction(valueRecord.RoundStartTime, valueRecord.RoundEndTime, cellBegin, cellEnd);
      if (coeff >= 1.0)
        payoutAccs[i].merge(valueRecord, CoinInfo_.FractionalPartSize);
      else
        payoutAccs[i].mergeScaled(valueRecord, coeff, CoinInfo_.FractionalPartSize);

      cellBegin = cellEnd;
      cellEnd += groupBy;
    }

    It->prev<CPPLNSPayout>(resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  for (auto &p : payoutAccs) {
    p.AvgHashRate = CoinInfo_.calculateAveragePower(p.TotalIncomingWork, groupByInterval, p.PrimePOWTarget);
  }

  callback(payoutAccs);
}

void AccountingDb::poolLuckImpl(const std::vector<int64_t> &intervals, PoolLuckCallback callback)
{
  if (!std::is_sorted(intervals.begin(), intervals.end())) {
    callback(std::vector<double>());
    return;
  }

  Timestamp currentTime = Timestamp::now();
  std::vector<double> result;

  auto &db = getFoundBlocksDb();
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());
  It->seekLast();

  auto intervalIt = intervals.begin();
  if (intervalIt == intervals.end()) {
    callback(result);
    return;
  }

  UInt<256> acceptedWork = UInt<256>::zero();
  UInt<256> expectedWork = UInt<256>::zero();
  for (const auto &score: State_.CurrentScores)
    acceptedWork += score.second;

  Timestamp currentTimePoint = currentTime - std::chrono::seconds(*intervalIt);
  while (It->valid()) {
    FoundBlockRecord dbBlock;
    RawData data = It->value();
    if (!dbBlock.deserializeValue(data.data, data.size))
      break;

    while (dbBlock.Time < currentTimePoint) {
      result.push_back(expectedWork.nonZero() ? UInt<256>::fpdiv(acceptedWork, expectedWork) : 0.0);
      if (++intervalIt == intervals.end()) {
        callback(result);
        return;
      }

      currentTimePoint = currentTime - std::chrono::seconds(*intervalIt);
    }

    if (dbBlock.ExpectedWork.nonZero()) {
      acceptedWork += dbBlock.AccumulatedWork;
      expectedWork += dbBlock.ExpectedWork;
    }

    It->prev();
  }

  while (intervalIt++ != intervals.end())
    result.push_back(expectedWork.nonZero() ? UInt<256>::fpdiv(acceptedWork, expectedWork) : 0.0);
  callback(result);
}

void AccountingDb::queryBalanceImpl(const std::string &user, QueryBalanceCallback callback)
{
  UserBalanceInfo info;

  // Calculate queued balance
  info.Queued = UInt<384>::zero();
  for (const auto &It: UnpayedRounds_) {
    auto payout = std::lower_bound(It->Payouts.begin(), It->Payouts.end(), user, [](const CUserPayout &record, const std::string &user) -> bool { return record.UserId < user; });
    if (payout != It->Payouts.end() && payout->UserId == user)
      info.Queued += payout->Value;
  }
  auto &balanceMap = getUserBalanceMap();
  auto It = balanceMap.find(user);
  if (It != balanceMap.end()) {
    info.Data = It->second;
  } else {
    info.Data.Login = user;
    info.Data.Balance = UInt<384>::zero();
    info.Data.Requested = UInt<384>::zero();
    info.Data.Paid = UInt<384>::zero();
  }

  callback(info);
}
