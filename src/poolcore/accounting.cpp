#include "poolcore/accounting.h"
#include "poolcore/feeEstimator.h"
#include "poolcommon/coroutineJoin.h"
#include "poolcommon/debug.h"
#include "poolcommon/mergeSorted.h"
#include "poolcommon/utils.h"
#include "poolcore/priceFetcher.h"
#include "loguru.hpp"
#include <math.h>
#include <thread>

static const UInt<384> ASYNC_RPC_OPERATION_DEFAULT_MINERS_FEE = fromRational(10000u);

namespace {
const std::string defaultFeePlan = "default";
const std::vector<::UserFeePair> emptyFeeRecord;
} // namespace

template<> struct DbIo<CPPSConfig> {
  static inline void serialize(xmstream &dst, const CPPSConfig &data) {
    DbIo<bool>::serialize(dst, data.Enabled);
    DbIo<double>::serialize(dst, data.PoolFee);
    DbIo<uint32_t>::serialize(dst, static_cast<uint32_t>(data.SaturationFunction));
    DbIo<double>::serialize(dst, data.SaturationB0);
    DbIo<double>::serialize(dst, data.SaturationANegative);
    DbIo<double>::serialize(dst, data.SaturationAPositive);
  }

  static inline void unserialize(xmstream &src, CPPSConfig &data) {
    DbIo<bool>::unserialize(src, data.Enabled);
    DbIo<double>::unserialize(src, data.PoolFee);
    uint32_t saturationFunction = 0;
    DbIo<uint32_t>::unserialize(src, saturationFunction);
    data.SaturationFunction = static_cast<ESaturationFunction>(saturationFunction);
    DbIo<double>::unserialize(src, data.SaturationB0);
    DbIo<double>::unserialize(src, data.SaturationANegative);
    DbIo<double>::unserialize(src, data.SaturationAPositive);
  }
};

template<> struct DbIo<CPPSBalanceSnapshot> {
  static inline void serialize(xmstream &dst, const CPPSBalanceSnapshot &data) {
    DbIo<UInt<384>>::serialize(dst, data.Balance);
    DbIo<double>::serialize(dst, data.TotalBlocksFound);
    DbIo<Timestamp>::serialize(dst, data.Time);
  }

  static inline void unserialize(xmstream &src, CPPSBalanceSnapshot &data) {
    DbIo<UInt<384>>::unserialize(src, data.Balance);
    DbIo<double>::unserialize(src, data.TotalBlocksFound);
    DbIo<Timestamp>::unserialize(src, data.Time);
  }
};

template<> struct DbIo<CPPSState> {
  static inline void serialize(xmstream &dst, const CPPSState &data) {
    DbIo<UInt<384>>::serialize(dst, data.Balance);
    DbIo<UInt<384>>::serialize(dst, data.LastBaseBlockReward);
    DbIo<double>::serialize(dst, data.TotalBlocksFound);
    DbIo<CPPSBalanceSnapshot>::serialize(dst, data.Min);
    DbIo<CPPSBalanceSnapshot>::serialize(dst, data.Max);
    DbIo<double>::serialize(dst, data.LastSaturateCoeff);
    DbIo<UInt<384>>::serialize(dst, data.LastAverageTxFee);
    DbIo<Timestamp>::serialize(dst, data.Time);
  }

  static inline void unserialize(xmstream &src, CPPSState &data) {
    DbIo<UInt<384>>::unserialize(src, data.Balance);
    DbIo<UInt<384>>::unserialize(src, data.LastBaseBlockReward);
    DbIo<double>::unserialize(src, data.TotalBlocksFound);
    DbIo<CPPSBalanceSnapshot>::unserialize(src, data.Min);
    DbIo<CPPSBalanceSnapshot>::unserialize(src, data.Max);
    DbIo<double>::unserialize(src, data.LastSaturateCoeff);
    DbIo<UInt<384>>::unserialize(src, data.LastAverageTxFee);
    DbIo<Timestamp>::unserialize(src, data.Time);
  }
};

void CPPSState::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, Time);
}

void CPPSState::serializeValue(xmstream &stream) const
{
  DbIo<CPPSState>::serialize(stream, *this);
}

bool CPPSState::deserializeValue(const void *data, size_t size)
{
  xmstream stream(const_cast<void*>(data), size);
  DbIo<CPPSState>::unserialize(stream, *this);
  return !stream.eof();
}

void AccountingDb::CPPLNSPayoutAcc::merge(const CPPLNSPayout &record, unsigned fractionalPartSize)
{
  TotalCoin += record.PayoutValue;

  UInt<384> btcValue = record.PayoutValue;
  btcValue.mulfp(record.RateToBTC * std::pow(10.0, 8 - static_cast<int>(fractionalPartSize)));
  TotalBTC += btcValue;

  UInt<384> usdValue = record.PayoutValue;
  usdValue.mulfp(record.RateToBTC * record.RateBTCToUSD / std::pow(10.0, fractionalPartSize));
  TotalUSD += usdValue;
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
}

void AccountingDb::CPPSPayoutAcc::merge(const CPPSPayout &record, unsigned fractionalPartSize)
{
  TotalCoin += record.PayoutValue;

  UInt<384> btcValue = record.PayoutValue;
  btcValue.mulfp(record.RateToBTC * std::pow(10.0, 8 - static_cast<int>(fractionalPartSize)));
  TotalBTC += btcValue;

  UInt<384> usdValue = record.PayoutValue;
  usdValue.mulfp(record.RateToBTC * record.RateBTCToUSD / std::pow(10.0, fractionalPartSize));
  TotalUSD += usdValue;
}

void AccountingDb::CPPSPayoutAcc::mergeScaled(const CPPSPayout &record, double coeff, unsigned fractionalPartSize)
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
}

bool CPPSConfig::parseSaturationFunction(const std::string &name, ESaturationFunction *out)
{
  struct { const char *name; ESaturationFunction value; } table[] = {
    {"none",     ESaturationFunction::None},
    {"tanh",     ESaturationFunction::Tangent},
    {"clamp",    ESaturationFunction::Clamp},
    {"cubic",    ESaturationFunction::Cubic},
    {"softsign", ESaturationFunction::Softsign},
    {"norm",     ESaturationFunction::Norm},
    {"atan",     ESaturationFunction::Atan},
    {"exp",      ESaturationFunction::Exp},
  };

  if (name.empty()) {
    *out = ESaturationFunction::None;
    return true;
  }

  for (const auto &entry : table) {
    if (name == entry.name) {
      *out = entry.value;
      return true;
    }
  }

  return false;
}

const char *CPPSConfig::saturationFunctionName(ESaturationFunction value)
{
  switch (value) {
    case ESaturationFunction::None:     return "none";
    case ESaturationFunction::Tangent:  return "tanh";
    case ESaturationFunction::Clamp:    return "clamp";
    case ESaturationFunction::Cubic:    return "cubic";
    case ESaturationFunction::Softsign: return "softsign";
    case ESaturationFunction::Norm:     return "norm";
    case ESaturationFunction::Atan:     return "atan";
    case ESaturationFunction::Exp:      return "exp";
    default:                            return "unknown";
  }
}

const std::vector<const char*> &CPPSConfig::saturationFunctionNames()
{
  static const std::vector<const char*> names = {
    "none", "tanh", "clamp", "cubic", "softsign", "norm", "atan", "exp"
  };
  return names;
}

double CPPSConfig::saturateCoeff(double balanceInBlocks) const
{
  if (SaturationFunction == ESaturationFunction::None)
    return 1.0;

  double x = (SaturationB0 > 0.0) ? balanceInBlocks / SaturationB0 : 0.0;
  double s;
  switch (SaturationFunction) {
    case ESaturationFunction::Tangent:
      s = std::tanh(x);
      break;
    case ESaturationFunction::Clamp:
      s = std::max(-1.0, std::min(1.0, x));
      break;
    case ESaturationFunction::Cubic:
      if (x <= -1.0) s = -1.0;
      else if (x >= 1.0) s = 1.0;
      else s = (3.0 * x - x * x * x) * 0.5;
      break;
    case ESaturationFunction::Softsign:
      s = x / (1.0 + std::abs(x));
      break;
    case ESaturationFunction::Norm:
      s = x / std::sqrt(1.0 + x * x);
      break;
    case ESaturationFunction::Atan:
      s = (2.0 / M_PI) * std::atan(x);
      break;
    case ESaturationFunction::Exp: {
      double ax = std::abs(x);
      s = 1.0 - std::exp(-ax);
      if (x < 0.0) s = -s;
      break;
    }
    default:
      s = 0.0;
      break;
  }

  double a = (x >= 0.0) ? SaturationAPositive : SaturationANegative;
  return 1.0 + a * s;
}

double CPPSState::balanceInBlocks(const UInt<384> &balance, const UInt<384> &baseBlockReward)
{
  if (baseBlockReward.isZero())
    return 0.0;
  bool neg = balance.isNegative();
  UInt<384> abs = balance;
  if (neg)
    abs.negate();
  double result = UInt<384>::fpdiv(abs, baseBlockReward);
  return neg ? -result : result;
}

double CPPSState::sqLambda(
  const UInt<384> &balance,
  const UInt<384> &baseBlockReward,
  double totalBlocksFound)
{
  if (totalBlocksFound <= 0.0)
    return 0.0;
  return balanceInBlocks(balance, baseBlockReward) / std::sqrt(totalBlocksFound);
}

static bool signedLess(const UInt<384> &a, const UInt<384> &b)
{
  bool aNeg = a.isNegative();
  bool bNeg = b.isNegative();
  if (aNeg != bNeg) return aNeg;
  return a < b;
}

void CPPSState::updateMinMax(Timestamp now)
{
  if (signedLess(Balance, Min.Balance)) {
    Min.Balance = Balance;
    Min.TotalBlocksFound = TotalBlocksFound;
    Min.Time = now;
  }
  if (signedLess(Max.Balance, Balance)) {
    Max.Balance = Balance;
    Max.TotalBlocksFound = TotalBlocksFound;
    Max.Time = now;
  }
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
  : DbPath_(dbPath),
    Db(dbPath / "accounting.state")
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
    } else if (keyStr == "ppspending") {
      DbIo<decltype(PPSPendingBalance)>::unserialize(stream, PPSPendingBalance);
    } else if (keyStr == "currentroundstart") {
      DbIo<Timestamp>::unserialize(stream, CurrentRoundStartTime);
    } else if (keyStr == "ppsconfig") {
      CPPSConfig cfg;
      DbIo<CPPSConfig>::unserialize(stream, cfg);
      PPSConfig.store(cfg, std::memory_order_relaxed);
    } else if (keyStr == "ppsstate") {
      DbIo<CPPSState>::unserialize(stream, PPSState);
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

  LastAcceptedMsgId_ = SavedShareId;

  if (SavedShareId != 0) {
    LOG_F(INFO, "AccountingDb: loaded state from db, SavedShareId=%" PRIu64 "", SavedShareId);
    return true;
  }
  return false;
}

void AccountingDb::CPersistentState::flushState()
{
  SavedShareId = LastAcceptedMsgId_;
  rocksdbBase::CBatch batch = Db.batch("default");

  // Save LastAcceptedMsgId_
  {
    xmstream stream;
    DbIo<uint64_t>::serialize(stream, LastAcceptedMsgId_);
    std::string key = "lastmsgid";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }

  // CurrentScores: save if non-empty, delete if cleared (e.g. after block found)
  {
    std::string key = "currentscores";
    if (CurrentScores.empty()) {
      batch.deleteRow(key.data(), key.size());
    } else {
      xmstream stream;
      DbIo<decltype(CurrentScores)>::serialize(stream, CurrentScores);
      batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
    }
  }

  // PPSPendingBalance: save if non-empty, delete if cleared (e.g. after PPS payout)
  {
    std::string key = "ppspending";
    if (PPSPendingBalance.empty()) {
      batch.deleteRow(key.data(), key.size());
    } else {
      xmstream stream;
      DbIo<decltype(PPSPendingBalance)>::serialize(stream, PPSPendingBalance);
      batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
    }
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

  // Save PPS state
  {
    xmstream stream;
    DbIo<CPPSState>::serialize(stream, PPSState);
    std::string key = "ppsstate";
    batch.put(key.data(), key.size(), stream.data(), stream.sizeOf());
  }

  Db.writeBatch(batch);
}

void AccountingDb::CPersistentState::flushPPSConfig()
{
  xmstream stream;
  DbIo<CPPSConfig>::serialize(stream, PPSConfig.load(std::memory_order_relaxed));
  Db.put("default", "ppsconfig", 9, stream.data(), stream.sizeOf());
}

void AccountingDb::CPersistentState::flushPayoutQueue()
{
  xmstream stream;
  for (auto &p: PayoutQueue)
    p.serializeValue(stream);

  std::string key = "payoutqueue";
  Db.put("default", key.data(), key.size(), stream.data(), stream.sizeOf());
}

void AccountingDb::CPersistentState::applyBatch(uint64_t msgId, const CAccountingStateBatch &batch)
{
  if (msgId <= LastAcceptedMsgId_)
    return;
  LastAcceptedMsgId_ = msgId;
  PPSState.LastSaturateCoeff = batch.LastSaturateCoeff;
  PPSState.LastBaseBlockReward = batch.LastBaseBlockReward;
  PPSState.LastAverageTxFee = batch.LastAverageTxFee;
  for (const auto &[user, work] : batch.PPLNSScores)
    CurrentScores[user] += work;
  for (const auto &[user, amount] : batch.PPSBalances)
    PPSPendingBalance[user] += amount;
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
  State_(config.dbPath),
  _roundsDb(config.dbPath / "rounds.3"),
  _balanceDb(config.dbPath / "balance.2"),
  _foundBlocksDb(config.dbPath / "foundBlocks.2"),
  _poolBalanceDb(config.dbPath / "poolBalance.2"),
  _payoutDb(config.dbPath / "payouts.2"),
  PPLNSPayoutsDb(config.dbPath / "pplns.payouts.2"),
  PPSPayoutsDb(config.dbPath / "pps.payouts"),
  PPSHistoryDb_(config.dbPath / "pps.history"),
  ShareLog_(config.dbPath / "accounting.worklog", coinInfo.Name, config.ShareLogFileSizeLimit),
  UserStatsAcc_("accounting.userstats", config.StatisticUserGridInterval, config.AccountingPPLNSWindow * 2),
  TaskHandler_(this, base)
{
  FlushTimerEvent_ = newUserEvent(base, 1, nullptr, nullptr);
  ShareLogFlushEvent_ = newUserEvent(base, 1, nullptr, nullptr);
  UserStatsFlushEvent_ = newUserEvent(base, 1, nullptr, nullptr);
  PPSPayoutEvent_ = newUserEvent(base, 1, nullptr, nullptr);

  // Start loading user data from UserManager in parallel with local DB loading
  std::thread settingsThread([this]() {
    UserManager_.fillUserCoinSettings(CoinInfo_.Name, UserSettings_);
    LOG_F(INFO, "AccountingDb: loaded %zu user settings for %s", UserSettings_.size(), CoinInfo_.Name.c_str());
  });

  std::thread feePlanIdsThread([this]() {
    UserManager_.fillUserFeePlanIds(UserFeePlanIds_);
    LOG_F(INFO, "AccountingDb: loaded %zu user fee plan assignments for %s", UserFeePlanIds_.size(), CoinInfo_.Name.c_str());
  });

  std::thread feePlansThread([this]() {
    for (unsigned m = 0; m < static_cast<unsigned>(EMiningMode::Count); ++m) {
      EMiningMode mode = static_cast<EMiningMode>(m);
      auto records = UserManager_.getAllFeeRecords(mode, CoinInfo_.Name);
      for (auto &[feePlanId, feeList] : records)
        FeePlanCache_[{feePlanId, mode}] = std::move(feeList);
    }
    LOG_F(INFO, "AccountingDb: loaded %zu fee plan entries for %s", FeePlanCache_.size(), CoinInfo_.Name.c_str());
  });

  UserStatsAcc_.load(config.dbPath, coinInfo.Name);
  State_.load();
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

  // Wait for user data loading threads before replay
  // (replay needs UserSettings_, UserFeePlanIds_, FeePlanCache_ for PPS mode handling)
  settingsThread.join();
  feePlanIdsThread.join();
  feePlansThread.join();

  {
    uint64_t replayFrom = std::min(State_.lastAcceptedMsgId(), UserStatsAcc_.lastSavedMsgId());
    ShareLog_.replay([this, replayFrom](uint64_t messageId, const CUserWorkSummaryBatch &batch) {
      if (messageId <= replayFrom)
        return;
      auto processed = processWorkSummaryBatch(batch);
      State_.applyBatch(messageId, processed.AccountingBatch);
      UserStatsAcc_.addBaseWorkBatch(messageId, processed.StatsBatch);
    });
  }

  printRecentStatistic();
  if (!State_.CurrentScores.empty()) {
    LOG_F(INFO, "[%s] current scores:", CoinInfo_.Name.c_str());
    for (const auto &It: State_.CurrentScores)
      LOG_F(INFO, " * %s: %s", It.first.c_str(), It.second.getDecimal().c_str());
  } else {
    LOG_F(INFO, "[%s] current scores is empty", CoinInfo_.Name.c_str());
  }
  // Flush replayed data immediately so AccumulationInterval_ is reset.
  // Otherwise, if the pool was down for a long time, the first live share
  // would stretch the interval and corrupt grid distribution.
  UserStatsAcc_.flush(Timestamp::now(), _cfg.dbPath, nullptr);

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

void AccountingDb::ppsPayout()
{
  if (State_.PPSPendingBalance.empty())
    return;

  LOG_F(INFO, "[%s] PPS payout: %zu users", CoinInfo_.Name.c_str(), State_.PPSPendingBalance.size());

  Timestamp now = Timestamp::now();
  Timestamp intervalBegin = now - _cfg.PPSPayoutInterval;
  double rateToBTC = PriceFetcher_.getPrice(CoinInfo_.Name);
  double rateBTCToUSD = PriceFetcher_.getBtcUsd();

  UInt<384> totalPaid = UInt<384>::zero();
  kvdb<rocksdbBase>::Batch batch;
  for (auto &[userId, value] : State_.PPSPendingBalance) {
    auto balanceIt = _balanceMap.find(userId);
    if (balanceIt == _balanceMap.end())
      balanceIt = _balanceMap.emplace(userId, UserBalanceRecord(userId, _cfg.DefaultPayoutThreshold)).first;

    UserBalanceRecord &balance = balanceIt->second;
    balance.Balance += value;
    balance.PPSPaid += value;
    totalPaid += value;
    batch.put(balance);

    {
      CPPSPayout record;
      record.Login = userId;
      record.IntervalBegin = intervalBegin;
      record.IntervalEnd = now;
      record.PayoutValue = value;
      record.RateToBTC = rateToBTC;
      record.RateBTCToUSD = rateBTCToUSD;
      PPSPayoutsDb.put(record);
    }

    LOG_F(INFO, " * %s: +%s", userId.c_str(), FormatMoneyFull(value, CoinInfo_.FractionalPartSize).c_str());
  }

  State_.PPSState.Balance -= totalPaid;
  State_.PPSState.updateMinMax(now);
  LOG_F(INFO,
        "[%s] PPS payout total: %s",
        CoinInfo_.Name.c_str(),
        FormatMoney(totalPaid, CoinInfo_.FractionalPartSize).c_str());

  _balanceDb.writeBatch(batch);
  State_.PPSPendingBalance.clear();
  State_.PPSState.Time = now;
  State_.flushState();
  PPSHistoryDb_.put(State_.PPSState);
}

void AccountingDb::userStatsFlushHandler()
{
  for (;;) {
    ioSleep(UserStatsFlushEvent_, std::chrono::microseconds(_cfg.StatisticUserFlushInterval).count());
    UserStatsAcc_.flush(Timestamp::now(), _cfg.dbPath, nullptr);
    if (ShutdownRequested_)
      break;
  }
}

void AccountingDb::ppsPayoutHandler()
{
  for (;;) {
    ioSleep(PPSPayoutEvent_, std::chrono::microseconds(_cfg.PPSPayoutInterval).count());
    if (ShutdownRequested_)
      break;
    ppsPayout();
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
      db->State_.flushState();
      if (db->ShutdownRequested_)
        break;
    }
  }, this, 0x20000, coroutineFinishCb, &FlushFinished_));

  coroutineCall(coroutineNewWithCb([](void *arg) {
    static_cast<AccountingDb*>(arg)->userStatsFlushHandler();
  }, this, 0x20000, coroutineFinishCb, &UserStatsFlushFinished_));

  coroutineCall(coroutineNewWithCb([](void *arg) {
    static_cast<AccountingDb*>(arg)->ppsPayoutHandler();
  }, this, 0x100000, coroutineFinishCb, &PPSPayoutFinished_));
}

void AccountingDb::stop()
{
  TaskHandler_.stop(CoinInfo_.Name.c_str(), "accounting: task handler");
  ShutdownRequested_ = true;
  userEventActivate(FlushTimerEvent_);
  userEventActivate(ShareLogFlushEvent_);
  userEventActivate(UserStatsFlushEvent_);
  userEventActivate(PPSPayoutEvent_);
  coroutineJoin(CoinInfo_.Name.c_str(), "accounting: flush thread", &FlushFinished_);
  coroutineJoin(CoinInfo_.Name.c_str(), "accounting: share log flush", &ShareLogFlushFinished_);
  coroutineJoin(CoinInfo_.Name.c_str(), "accounting: user stats flush", &UserStatsFlushFinished_);
  coroutineJoin(CoinInfo_.Name.c_str(), "accounting: pps payout", &PPSPayoutFinished_);
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

void AccountingDb::calculatePPLNSPayments(MiningRound *R, const UInt<384> &generatedCoins)
{
  R->AvailableCoins = generatedCoins;
  R->Payouts.clear();

  if (R->TotalShareValue.isZero()) {
    LOG_F(ERROR, "Block found but TotalShareValue is zero, skipping payouts");
    return;
  }

  // Deduct PPS meta-user's share from the block reward before PPLNS distribution
  auto ppsMetaIt = std::find_if(R->UserShares.begin(), R->UserShares.end(),
    [](const UserShareValue &s) { return s.UserId == ppsMetaUserId(); });
  if (ppsMetaIt != R->UserShares.end()) {
    double ppsFraction = UInt<256>::fpdiv(ppsMetaIt->ShareValue, R->TotalShareValue);
    UInt<384> ppsShare = R->AvailableCoins / R->TotalShareValue;
    ppsShare *= ppsMetaIt->ShareValue;
    R->AvailableCoins -= ppsShare;
    R->TotalShareValue -= ppsMetaIt->ShareValue;
    R->UserShares.erase(ppsMetaIt);
    State_.PPSState.Balance += ppsShare;
    State_.PPSState.TotalBlocksFound += ppsFraction;
    State_.PPSState.updateMinMax(Timestamp::now());
    LOG_F(INFO,
          " * PPS deduction: %s (%.3f of block, remaining for PPLNS: %s)",
          FormatMoney(ppsShare, CoinInfo_.FractionalPartSize).c_str(),
          ppsFraction,
          FormatMoney(R->AvailableCoins, CoinInfo_.FractionalPartSize).c_str());
  }

  if (R->TotalShareValue.isZero()) {
    LOG_F(WARNING, "Block found but no PPLNS shares, all work was PPS");
    return;
  }

  static const std::string defaultFeePlan = "default";
  static const std::vector<::UserFeePair> emptyFeeRecord;
  UInt<384> totalPayout = UInt<384>::zero();
  std::vector<CUserPayout> payouts;
  std::map<std::string, UInt<384>> feePayouts;
  UInt<384> minShareCost = R->AvailableCoins / R->TotalShareValue;
  for (const auto &record: R->UserShares) {
    // Calculate payout
    UInt<384> payoutValue = minShareCost * record.ShareValue;
    totalPayout += payoutValue;

    // get fee plan for user
    auto feePlanIt = UserFeePlanIds_.find(record.UserId);
    const std::string &feePlanId = feePlanIt != UserFeePlanIds_.end() ? feePlanIt->second : defaultFeePlan;
    auto cacheIt = FeePlanCache_.find({feePlanId, EMiningMode::PPLNS});
    const std::vector<::UserFeePair> &feeRecord = cacheIt != FeePlanCache_.end() ? cacheIt->second : emptyFeeRecord;

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
    bool needSubtract = totalPayout > R->AvailableCoins;

    if (needSubtract) {
      diff = totalPayout - R->AvailableCoins;
    } else {
      diff = R->AvailableCoins - totalPayout;
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

void AccountingDb::onUserWorkSummary(const CUserWorkSummaryBatch &batch)
{
  if (batch.Time.TimeBegin > batch.Time.TimeEnd ||
      (batch.Time.TimeEnd - batch.Time.TimeBegin) > MaxBatchTimeInterval) {
    LOG_F(ERROR,
          "AccountingDb::onUserWorkSummary: invalid batch time [%" PRId64 ", %" PRId64 "], %zu entries dropped",
          batch.Time.TimeBegin.count(),
          batch.Time.TimeEnd.count(),
          batch.Entries.size());
    return;
  }

  uint64_t messageId = ShareLog_.addMessage(batch);
  auto processed = processWorkSummaryBatch(batch);
  State_.applyBatch(messageId, processed.AccountingBatch);
  UserStatsAcc_.addBaseWorkBatch(messageId, processed.StatsBatch);
}

CProcessedWorkSummary AccountingDb::processWorkSummaryBatch(const CUserWorkSummaryBatch &batch)
{
  CProcessedWorkSummary result;
  result.StatsBatch.Time = batch.Time;

  // Special meta-user for aggregating PPS shares into PPLNS scoring
  CUserWorkSummary ppsMeta;
  ppsMeta.UserId = ppsMetaUserId();

  CPPSConfig ppsConfig = State_.PPSConfig.load(std::memory_order_relaxed);
  bool ppsEnabled = ppsConfig.Enabled;

  // Determine LastBaseBlockReward from batch (all entries share the same value)
  UInt<384> lastBaseBlockReward = State_.PPSState.LastBaseBlockReward;
  for (const auto &entry : batch.Entries) {
    if (!entry.BaseBlockReward.isZero()) {
      lastBaseBlockReward = entry.BaseBlockReward;
      break;
    }
  }
  result.AccountingBatch.LastBaseBlockReward = lastBaseBlockReward;

  // Compute saturation coefficient once per batch
  double saturateCoeff = 1.0;
  double ppsPoolFee = ppsConfig.PoolFee;
  UInt<384> averageTxFee = State_.PPSState.LastAverageTxFee;
  if (ppsEnabled) {
    double currentBalanceInBlocks =
      CPPSState::balanceInBlocks(State_.PPSState.Balance, lastBaseBlockReward);
    saturateCoeff = ppsConfig.saturateCoeff(currentBalanceInBlocks);
    if (FeeEstimationService_)
      averageTxFee = FeeEstimationService_->averageFee();
  }
  result.AccountingBatch.LastSaturateCoeff = saturateCoeff;
  result.AccountingBatch.LastAverageTxFee = averageTxFee;

  for (const auto &entry : batch.Entries) {
    EMiningMode mode = EMiningMode::PPLNS;
    if (ppsEnabled) {
      auto settingsIt = UserSettings_.find(entry.UserId);
      if (settingsIt != UserSettings_.end())
        mode = settingsIt->second.MiningMode;
    }

    if (mode == EMiningMode::PPLNS) {
      // PPLNS: add shares to scores and user stats
      result.AccountingBatch.PPLNSScores.emplace_back(entry.UserId, entry.AcceptedWork);
      result.StatsBatch.Entries.push_back({entry.UserId, entry.AcceptedWork, entry.SharesNum, {}, {}});
    } else {
      // PPS: aggregate shares for meta-user, calculate reward
      ppsMeta.SharesNum += entry.SharesNum;
      ppsMeta.AcceptedWork += entry.AcceptedWork;

      if (!entry.ExpectedWork.isZero()) {
        // Fixed-point 128.256: high 128 bits = integer satoshi, low 256 bits = fractional
        UInt<384> batchCost = lastBaseBlockReward + averageTxFee;
        batchCost /= entry.ExpectedWork;
        batchCost *= entry.AcceptedWork;
        batchCost.mulfp(saturateCoeff);

        // Pool fee (stays in PPS balance, not distributed)
        UInt<384> poolFeeValue = batchCost;
        poolFeeValue.mulfp(ppsPoolFee / 100.0);

        // User fee plan (distributed to fee recipients)
        auto feePlanIt = UserFeePlanIds_.find(entry.UserId);
        const std::string &feePlanId =
          feePlanIt != UserFeePlanIds_.end() ? feePlanIt->second : defaultFeePlan;
        auto cacheIt = FeePlanCache_.find({feePlanId, EMiningMode::PPS});
        const auto &feeRecord =
          cacheIt != FeePlanCache_.end() ? cacheIt->second : emptyFeeRecord;

        UInt<384> userFeeSum = UInt<384>::zero();
        std::vector<UInt<384>> feeValues;
        for (const auto &fee : feeRecord) {
          UInt<384> feeValue = batchCost;
          feeValue.mulfp(fee.Percentage / 100.0);
          feeValues.push_back(feeValue);
          userFeeSum += feeValue;
        }

        UInt<384> totalFeeSum = poolFeeValue + userFeeSum;
        if (totalFeeSum <= batchCost) {
          for (size_t i = 0, ie = feeRecord.size(); i != ie; ++i)
            result.AccountingBatch.PPSBalances.emplace_back(feeRecord[i].UserId, feeValues[i]);
          result.AccountingBatch.PPSBalances.emplace_back(entry.UserId, batchCost - totalFeeSum);
        } else {
          double userFeePct = 0.0;
          for (const auto &fee : feeRecord)
            userFeePct += fee.Percentage;
          LOG_F(ERROR,
            "PPS user %s: fee sum (pool %.2f%% + user %.2f%%) exceeds 100%%, fees not applied",
            entry.UserId.c_str(),
            ppsPoolFee,
            userFeePct);
          result.AccountingBatch.PPSBalances.emplace_back(entry.UserId, batchCost);
        }
      }
    }
  }

  // Add aggregated PPS shares to scores and user stats under meta-user
  if (ppsMeta.SharesNum) {
    result.AccountingBatch.PPLNSScores.emplace_back(ppsMeta.UserId, ppsMeta.AcceptedWork);
    result.StatsBatch.Entries.push_back(
      {ppsMeta.UserId, ppsMeta.AcceptedWork, ppsMeta.SharesNum, {}, {}});
  }

  return result;
}

void AccountingDb::onUserSettingsUpdate(const UserSettingsRecord &settings)
{
  UserSettings_[settings.Login] = settings;
}

void AccountingDb::onFeePlanUpdate(const std::string &feePlanId, EMiningMode mode, const std::vector<::UserFeePair> &feeRecord)
{
  FeePlanCache_[{feePlanId, mode}] = feeRecord;
  LOG_F(INFO, "AccountingDb %s: fee plan '%s' updated for mode %s, %zu entries",
    CoinInfo_.Name.c_str(), feePlanId.c_str(), miningModeName(mode), feeRecord.size());
}

void AccountingDb::onFeePlanDelete(const std::string &feePlanId)
{
  for (unsigned m = 0; m < static_cast<unsigned>(EMiningMode::Count); ++m)
    FeePlanCache_.erase({feePlanId, static_cast<EMiningMode>(m)});
  LOG_F(INFO, "AccountingDb %s: fee plan '%s' deleted", CoinInfo_.Name.c_str(), feePlanId.c_str());
}

void AccountingDb::onUserFeePlanChange(const std::string &login, const std::string &feePlanId)
{
  UserFeePlanIds_[login] = feePlanId;
  LOG_F(INFO, "AccountingDb %s: user '%s' fee plan changed to '%s'", CoinInfo_.Name.c_str(), login.c_str(), feePlanId.c_str());
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
  // the full PPLNS window — no re-export from UserStatsAcc is needed here.
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
    calculatePPLNSPayments(R, block.GeneratedCoins);

  // NOTE: crash between _roundsDb.put and flushState will leave
  // data inconsistent — round is persisted but State_ (CurrentScores,
  // RecentStats, CurrentRoundStartTime) is not updated. On restart the
  // old CurrentScores will be re-accumulated into the next round.

  // store round to DB and clear shares map
  _allRounds.emplace_back(R);
  _roundsDb.put(*R);
  if (!R->Payouts.empty())
    UnpayedRounds_.insert(R);

  // Query statistics
  UserStatsAcc_.exportRecentStats(_cfg.AccountingPPLNSWindow, State_.RecentStats);
  printRecentStatistic();

  // Reset aggregated data
  State_.CurrentScores.clear();
  State_.CurrentRoundStartTime = R->EndTime;

  // Save state to db
  State_.PPSState.Time = Timestamp::now();
  State_.flushState();
  PPSHistoryDb_.put(State_.PPSState);
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
      calculatePPLNSPayments(R, confirmationsQuery[i].BlockReward);
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
  auto settingsIt = UserSettings_.find(payout.UserId);
  if (settingsIt == UserSettings_.end() || settingsIt->second.Address.empty()) {
    LOG_F(WARNING, "user %s did not setup payout address, ignoring", payout.UserId.c_str());
    *needSkipPayout = true;
    return;
  }

  const UserSettingsRecord &settings = settingsIt->second;
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
  UInt<384> ppsPaid = UInt<384>::zero();
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
    ppsPaid += userIt.second.PPSPaid;
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

  if (State_.PPSConfig.load(std::memory_order_relaxed).Enabled) {
    const auto &pps = State_.PPSState;
    const auto &reward = pps.LastBaseBlockReward;
    double currentBalanceInBlocks = CPPSState::balanceInBlocks(pps.Balance, reward);
    double currentSqLambda = CPPSState::sqLambda(pps.Balance, reward, pps.TotalBlocksFound);
    double minBalanceInBlocks = CPPSState::balanceInBlocks(pps.Min.Balance, reward);
    double minSqLambda = CPPSState::sqLambda(pps.Min.Balance, reward, pps.Min.TotalBlocksFound);
    double maxBalanceInBlocks = CPPSState::balanceInBlocks(pps.Max.Balance, reward);
    double maxSqLambda = CPPSState::sqLambda(pps.Max.Balance, reward, pps.Max.TotalBlocksFound);
    LOG_F(INFO,
          "PPS state: balance=%s (%.3f blocks, sqLambda=%.4f),"
          " min=%s (%.3f blocks, sqLambda=%.4f),"
          " max=%s (%.3f blocks, sqLambda=%.4f),"
          " blocks=%.3f, saturateCoeff=%.4f, avgTxFee=%s",
          FormatMoney(pps.Balance, CoinInfo_.FractionalPartSize).c_str(),
          currentBalanceInBlocks,
          currentSqLambda,
          FormatMoney(pps.Min.Balance, CoinInfo_.FractionalPartSize).c_str(),
          minBalanceInBlocks,
          minSqLambda,
          FormatMoney(pps.Max.Balance, CoinInfo_.FractionalPartSize).c_str(),
          maxBalanceInBlocks,
          maxSqLambda,
          pps.TotalBlocksFound,
          pps.LastSaturateCoeff,
          FormatMoney(pps.LastAverageTxFee, CoinInfo_.FractionalPartSize).c_str());
  }
}

bool AccountingDb::requestPayout(const std::string &address, const UInt<384> &value, bool force)
{
  bool result = false;
  auto It = _balanceMap.find(address);
  if (It == _balanceMap.end())
    It = _balanceMap.insert(It, std::make_pair(address, UserBalanceRecord(address, _cfg.DefaultPayoutThreshold)));

  UserBalanceRecord &balance = It->second;
  balance.Balance += value;

  auto settingsIt = UserSettings_.find(balance.Login);
  UInt<384> nonQueuedBalance = balance.Balance - balance.Requested;
  if (!nonQueuedBalance.isNegative() && settingsIt != UserSettings_.end() &&
      (force || (settingsIt->second.AutoPayout && nonQueuedBalance >= settingsIt->second.MinimalPayout))) {
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

  callback(payoutAccs);
}

std::vector<AccountingDb::CPPSPayoutAcc> AccountingDb::queryPPSPayoutsAcc(
  const std::string &login,
  int64_t timeFrom,
  int64_t timeTo,
  int64_t groupByInterval)
{
  std::vector<CPPSPayoutAcc> payoutAccs;

  constexpr int64_t MaxOutputCells = 3200;
  if (timeTo <= timeFrom ||
      groupByInterval <= 0 ||
      (timeTo - timeFrom) % groupByInterval != 0 ||
      (timeTo - timeFrom) / groupByInterval > MaxOutputCells) {
    return payoutAccs;
  }

  auto &db = getPPSPayoutsDb();
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());

  CPPSPayout valueRecord;
  xmstream resumeKey;
  auto validPredicate = [&login](const CPPSPayout &record) -> bool {
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
    CPPSPayout record;
    record.Login = login;
    record.IntervalBegin = Timestamp(std::numeric_limits<int64_t>::max());
    record.serializeKey(resumeKey);
  }

  {
    CPPSPayout keyRecord;
    keyRecord.Login = login;
    keyRecord.IntervalBegin = gridEnd;
    It->seekForPrev<CPPSPayout>(keyRecord, resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  while (It->valid()) {
    if (valueRecord.IntervalEnd <= gridStart)
      break;

    Timestamp clampedBegin = std::max(valueRecord.IntervalBegin, gridStart);
    Timestamp clampedEnd = std::min(valueRecord.IntervalEnd, gridEnd);

    size_t firstIdx = static_cast<size_t>((clampedBegin - gridStart) / groupBy);
    size_t lastIdx = static_cast<size_t>((clampedEnd - gridStart + groupBy - std::chrono::milliseconds(1)) / groupBy);

    Timestamp cellBegin = gridStart + groupBy * static_cast<int64_t>(firstIdx);
    Timestamp cellEnd = cellBegin + groupBy;
    for (size_t i = firstIdx; i < lastIdx; i++) {
      double coeff = overlapFraction(valueRecord.IntervalBegin, valueRecord.IntervalEnd, cellBegin, cellEnd);
      if (coeff >= 1.0)
        payoutAccs[i].merge(valueRecord, CoinInfo_.FractionalPartSize);
      else
        payoutAccs[i].mergeScaled(valueRecord, coeff, CoinInfo_.FractionalPartSize);

      cellBegin = cellEnd;
      cellEnd += groupBy;
    }

    It->prev<CPPSPayout>(resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  return payoutAccs;
}

std::vector<CPPSPayout> AccountingDb::queryPPSPayouts(const std::string &login, int64_t timeFrom, uint32_t count)
{
  auto &db = getPPSPayoutsDb();
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());

  CPPSPayout valueRecord;
  xmstream resumeKey;
  auto validPredicate = [&login](const CPPSPayout &record) -> bool {
    return record.Login == login;
  };

  {
    CPPSPayout record;
    record.Login = login;
    record.IntervalBegin = Timestamp(std::numeric_limits<int64_t>::max());
    record.serializeKey(resumeKey);
  }

  {
    CPPSPayout keyRecord;
    keyRecord.Login = login;
    keyRecord.IntervalBegin = timeFrom == 0 ? Timestamp(std::numeric_limits<int64_t>::max()) : Timestamp::fromUnixTime(timeFrom);
    It->seekForPrev<CPPSPayout>(keyRecord, resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  std::vector<CPPSPayout> payouts;
  for (unsigned i = 0; i < count && It->valid(); i++) {
    payouts.emplace_back(valueRecord);
    It->prev<CPPSPayout>(resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  return payouts;
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

void AccountingDb::queryPPSConfigImpl(QueryPPSConfigCallback callback)
{
  callback(State_.PPSConfig.load(std::memory_order_relaxed));
}

void AccountingDb::queryPPSStateImpl(QueryPPSStateCallback callback)
{
  callback(State_.PPSState);
}

std::vector<CPPSState> AccountingDb::queryPPSHistory(int64_t timeFrom, int64_t timeTo)
{
  std::unique_ptr<rocksdbBase::IteratorType> It(PPSHistoryDb_.iterator());

  CPPSState searchKey;
  searchKey.Time = Timestamp::fromUnixTime(timeFrom);
  It->seek(searchKey);

  Timestamp limit = Timestamp::fromUnixTime(timeTo);
  std::vector<CPPSState> result;
  while (It->valid()) {
    CPPSState record;
    RawData data = It->value();
    if (!record.deserializeValue(data.data, data.size))
      break;
    if (record.Time > limit)
      break;
    result.emplace_back(record);
    It->next();
  }

  return result;
}

void AccountingDb::updatePPSConfigImpl(const CPPSConfig &cfg, DefaultCb callback)
{
  if (cfg.SaturationFunction != ESaturationFunction::None) {
    if (cfg.SaturationB0 <= 0.0 ||
        cfg.SaturationANegative < 0.0 || cfg.SaturationANegative > 1.0 ||
        cfg.SaturationAPositive < 0.0 || cfg.SaturationAPositive > 1.0) {
      callback("invalid_saturation_params");
      return;
    }
  }

  State_.PPSConfig.store(cfg, std::memory_order_relaxed);
  State_.flushPPSConfig();
  LOG_F(INFO,
    "[%s] PPS config updated: enabled=%d, poolFee=%.2f, saturation=%s, B0=%.4f, aNeg=%.4f, aPos=%.4f",
    CoinInfo_.Name.c_str(),
    static_cast<int>(cfg.Enabled),
    cfg.PoolFee,
    CPPSConfig::saturationFunctionName(cfg.SaturationFunction),
    cfg.SaturationB0,
    cfg.SaturationANegative,
    cfg.SaturationAPositive);
  callback("ok");
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
