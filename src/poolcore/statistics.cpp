#include "poolcore/statistics.h"
#include "poolcore/statsMergeOperator.h"
#include "poolcommon/coroutineJoin.h"
#include "poolcommon/debug.h"
#include "loguru.hpp"
#include <algorithm>

StatisticDb::StatisticDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo) : _cfg(config), CoinInfo_(coinInfo),
  StatsDb_(_cfg.dbPath / "statistic", std::make_shared<StatsRecordMergeOperator>()),
  TaskHandler_(this, base)
{
  FlushEvent_ = newUserEvent(base, 1, nullptr, nullptr);

  WorkerStats_.load(_cfg.dbPath, coinInfo.Name);
  UserStats_.load(_cfg.dbPath, coinInfo.Name);
  PoolStatsAcc_.load(_cfg.dbPath, coinInfo.Name);

  LastKnownShareId_ = std::max({WorkerStats_.lastShareId(), UserStats_.lastShareId(), PoolStatsAcc_.lastShareId()});
  if (isDebugStatistic())
    LOG_F(1, "%s: last aggregated id: %" PRIu64 " last known id: %" PRIu64 "", coinInfo.Name.c_str(), lastAggregatedShareId(), lastKnownShareId());
}

void StatisticDb::updatePoolStatsCached(Timestamp timeLabel)
{
  PoolStatsCached_.ClientsNum = 0;
  PoolStatsCached_.WorkersNum = 0;

  for (const auto &[key, acc]: UserStats_.map()) {
    if (timeLabel - acc.LastShareTime < std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticWorkersPowerCalculateInterval))
      PoolStatsCached_.ClientsNum++;
  }

  for (const auto &[key, acc]: WorkerStats_.map()) {
    if (timeLabel - acc.LastShareTime < std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticWorkersPowerCalculateInterval))
      PoolStatsCached_.WorkersNum++;
  }

  if (isDebugStatistic())
    LOG_F(1, "update pool stats:");
  PoolStatsAcc_.series().calcAverageMetrics(CoinInfo_, _cfg.StatisticPoolPowerCalculateInterval, _cfg.StatisticPoolGridInterval, PoolStatsCached_);

  LOG_F(INFO,
        "clients: %u, workers: %u, power: %" PRIu64 ", share rate: %.3lf shares/s",
        PoolStatsCached_.ClientsNum,
        PoolStatsCached_.WorkersNum,
        PoolStatsCached_.AveragePower,
        PoolStatsCached_.SharesPerSecond);
}

void StatisticDb::flushAll(Timestamp timeLabel)
{
  int64_t workerGridMs = std::chrono::duration_cast<std::chrono::milliseconds>(_cfg.StatisticWorkerGridInterval).count();
  int64_t userGridMs = std::chrono::duration_cast<std::chrono::milliseconds>(_cfg.StatisticUserGridInterval).count();
  int64_t poolGridMs = std::chrono::duration_cast<std::chrono::milliseconds>(_cfg.StatisticPoolGridInterval).count();
  Timestamp removeTimePoint = timeLabel - std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticKeepTime);

  kvdb<rocksdbBase>::Batch batch;
  std::set<int64_t> modifiedWorkerTimes;
  std::set<int64_t> modifiedUserTimes;
  std::set<int64_t> modifiedPoolTimes;
  std::set<int64_t> removedWorkerTimes;
  std::set<int64_t> removedUserTimes;
  std::set<int64_t> removedPoolTimes;

  // Flush workers
  WorkerStats_.flush(AccumulationBegin_.count(), timeLabel.count(), workerGridMs, removeTimePoint, timeLabel, LastKnownShareId_, &batch, modifiedWorkerTimes, removedWorkerTimes);

  // Flush users
  UserStats_.flush(AccumulationBegin_.count(), timeLabel.count(), userGridMs, removeTimePoint, timeLabel, LastKnownShareId_, &batch, modifiedUserTimes, removedUserTimes);

  // Flush pool
  PoolStatsAcc_.flush(AccumulationBegin_.count(), timeLabel.count(), poolGridMs, removeTimePoint, timeLabel, LastKnownShareId_, &batch, modifiedPoolTimes, removedPoolTimes);

  // Write batch to RocksDB
  StatsDb_.writeBatch(batch);

  // Rebuild .dat files for modified grid times
  for (int64_t gridEndMs : modifiedWorkerTimes)
    WorkerStats_.rebuildDatFile(_cfg.dbPath, gridEndMs);
  for (int64_t gridEndMs : modifiedUserTimes)
    UserStats_.rebuildDatFile(_cfg.dbPath, gridEndMs);
  for (int64_t gridEndMs : modifiedPoolTimes)
    PoolStatsAcc_.rebuildDatFile(_cfg.dbPath, gridEndMs);

  // Remove old .dat files
  for (int64_t gridEndMs : removedWorkerTimes)
    removeDatFile(_cfg.dbPath, WorkerStats_.cachePath(), gridEndMs);
  for (int64_t gridEndMs : removedUserTimes)
    removeDatFile(_cfg.dbPath, UserStats_.cachePath(), gridEndMs);
  for (int64_t gridEndMs : removedPoolTimes)
    removeDatFile(_cfg.dbPath, PoolStatsAcc_.cachePath(), gridEndMs);

  // Update cached pool stats
  updatePoolStatsCached(timeLabel);

  AccumulationBegin_ = timeLabel;
}

void StatisticDb::addShare(const CShare &share)
{
  bool isPrimePOW = CoinInfo_.PowerUnitType == CCoinInfo::ECPD;
  WorkerStats_.addShare(share.userId, share.workerId, share.WorkValue, share.Time, share.ChainLength, share.PrimePOWTarget, isPrimePOW);
  UserStats_.addShare(share.userId, "", share.WorkValue, share.Time, share.ChainLength, share.PrimePOWTarget, isPrimePOW);
  PoolStatsAcc_.addShare(share.WorkValue, share.Time, share.ChainLength, share.PrimePOWTarget, isPrimePOW);
  PoolStatsCached_.LastShareTime = share.Time;
  LastKnownShareId_ = std::max(LastKnownShareId_, share.UniqueShareId);
}

void StatisticDb::replayShare(const CShare &share)
{
  bool isPrimePOW = CoinInfo_.PowerUnitType == CCoinInfo::ECPD;
  if (share.UniqueShareId > WorkerStats_.lastShareId())
    WorkerStats_.addShare(share.userId, share.workerId, share.WorkValue, share.Time, share.ChainLength, share.PrimePOWTarget, isPrimePOW);
  if (share.UniqueShareId > UserStats_.lastShareId())
    UserStats_.addShare(share.userId, "", share.WorkValue, share.Time, share.ChainLength, share.PrimePOWTarget, isPrimePOW);
  if (share.UniqueShareId > PoolStatsAcc_.lastShareId())
    PoolStatsAcc_.addShare(share.WorkValue, share.Time, share.ChainLength, share.PrimePOWTarget, isPrimePOW);
  PoolStatsCached_.LastShareTime = share.Time;
  LastKnownShareId_ = std::max(LastKnownShareId_, share.UniqueShareId);

  if (isDebugStatistic()) {
    Dbg_.MinShareId = std::min(Dbg_.MinShareId, share.UniqueShareId);
    Dbg_.MaxShareId = std::max(Dbg_.MaxShareId, share.UniqueShareId);
    Dbg_.Count++;
  }
}

void StatisticDb::initializationFinish(Timestamp timeLabel)
{
  AccumulationBegin_ = timeLabel;

  if (isDebugStatistic())
    LOG_F(1, "%s: replayed %" PRIu64 " shares from %" PRIu64 " to %" PRIu64 "", CoinInfo_.Name.c_str(), Dbg_.Count, Dbg_.MinShareId, Dbg_.MaxShareId);
}

void StatisticDb::start()
{
  TaskHandler_.start();

  coroutineCall(coroutineNewWithCb([](void *arg) {
    StatisticDb *db = static_cast<StatisticDb*>(arg);
    for (;;) {
      ioSleep(db->FlushEvent_, std::chrono::microseconds(db->_cfg.StatisticFlushInterval).count());
      if (db->ShutdownRequested_)
        break;
      db->flushAll(Timestamp::now());
    }
  }, this, 0x20000, coroutineFinishCb, &FlushFinished_));
}

void StatisticDb::stop()
{
  ShutdownRequested_ = true;
  userEventActivate(FlushEvent_);
  TaskHandler_.stop(CoinInfo_.Name.c_str(), "statisticDb task handler");
  coroutineJoin(CoinInfo_.Name.c_str(), "statisticDb flush updater", &FlushFinished_);
}


void StatisticDb::getUserStats(const std::string &user, CStats &userStats, std::vector<CStats> &workerStats, size_t offset, size_t size, EStatsColumn sortBy, bool sortDescending)
{
  auto userIt = UserStats_.map().find(makeStatsKey(user, ""));
  auto workerLo = WorkerStats_.map().lower_bound(makeStatsKey(user, ""));
  bool hasWorkers = (workerLo != WorkerStats_.map().end() && splitStatsKey(workerLo->first).first == user);
  if (userIt == UserStats_.map().end() && !hasWorkers)
    return;

  userStats.ClientsNum = 1;
  userStats.WorkersNum = 0;

  std::vector<CStats> allStats;
  Timestamp lastShareTime;

  // User-level stats
  if (userIt != UserStats_.map().end()) {
    const CStatsSeries &acc = userIt->second;
    CStats &result = allStats.emplace_back();
    if (isDebugStatistic())
      LOG_F(1, "Retrieve statistic for %s/<user>", user.c_str());
    acc.calcAverageMetrics(CoinInfo_, _cfg.StatisticWorkersPowerCalculateInterval, _cfg.StatisticUserGridInterval, result);
    userStats.SharesPerSecond += result.SharesPerSecond;
    userStats.SharesWork += result.SharesWork;
    userStats.AveragePower += result.AveragePower;
    if (result.AveragePower)
      userStats.WorkersNum++;
    result.LastShareTime = acc.LastShareTime;
    lastShareTime = std::max(lastShareTime, acc.LastShareTime);
  }

  // Worker-level stats
  for (auto it = workerLo; it != WorkerStats_.map().end(); ++it) {
    auto [login, workerId] = splitStatsKey(it->first);
    if (login != user)
      break;
    const CStatsSeries &acc = it->second;
    CStats &result = allStats.emplace_back();
    result.WorkerId = std::move(workerId);
    if (isDebugStatistic())
      LOG_F(1, "Retrieve statistic for %s/%s", user.c_str(), result.WorkerId.c_str());
    acc.calcAverageMetrics(CoinInfo_, _cfg.StatisticWorkersPowerCalculateInterval, _cfg.StatisticWorkerGridInterval, result);
    userStats.SharesPerSecond += result.SharesPerSecond;
    userStats.SharesWork += result.SharesWork;
    userStats.AveragePower += result.AveragePower;
    if (result.AveragePower)
      userStats.WorkersNum++;
    result.LastShareTime = acc.LastShareTime;
    lastShareTime = std::max(lastShareTime, acc.LastShareTime);
  }

  userStats.LastShareTime = lastShareTime;

  // Build response
  // Sorting results
  switch (sortBy) {
    case EStatsColumnName :
      if (!sortDescending)
        std::sort(allStats.begin(), allStats.end(), [](const CStats &l, const CStats &r){ return l.WorkerId < r.WorkerId; });
      else
        std::sort(allStats.rbegin(), allStats.rend(), [](const CStats &l, const CStats &r){ return l.WorkerId < r.WorkerId; });
      break;
    case EStatsColumnAveragePower:
      if (!sortDescending)
        std::sort(allStats.begin(), allStats.end(), [](const CStats &l, const CStats &r){ return l.AveragePower < r.AveragePower; });
      else
        std::sort(allStats.rbegin(), allStats.rend(), [](const CStats &l, const CStats &r){ return l.AveragePower < r.AveragePower; });
      break;
    case EStatsColumnSharesPerSecond:
      if (!sortDescending)
        std::sort(allStats.begin(), allStats.end(), [](const CStats &l, const CStats &r){ return l.SharesPerSecond < r.SharesPerSecond; });
      else
        std::sort(allStats.rbegin(), allStats.rend(), [](const CStats &l, const CStats &r){ return l.SharesPerSecond < r.SharesPerSecond; });
      break;
    case EStatsColumnLastShareTime:
      if (!sortDescending)
        std::sort(allStats.begin(), allStats.end(), [](const CStats &l, const CStats &r){ return l.LastShareTime < r.LastShareTime; });
      else
        std::sort(allStats.rbegin(), allStats.rend(), [](const CStats &l, const CStats &r){ return l.LastShareTime < r.LastShareTime; });
      break;
  }

  // Make page
  if (offset < allStats.size()) {
    size_t endIdx = std::min(offset + size, allStats.size());
    workerStats.insert(workerStats.end(), std::make_move_iterator(allStats.begin() + offset), std::make_move_iterator(allStats.begin() + endIdx));
  }
}

void StatisticDb::getHistory(const std::string &login, const std::string &workerId, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, std::vector<CStats> &history)
{
  if (groupByInterval < 60)
    return;

  if (isDebugStatistic())
    LOG_F(1, "getHistory for %s/%s from %" PRIi64 " to %" PRIi64 " group interval %" PRIi64 "", login.c_str(), workerId.c_str(), timeFrom, timeTo, groupByInterval);
  std::unique_ptr<rocksdbBase::IteratorType> It(StatsDb_.iterator());

  StatsRecord valueRecord;
  xmstream resumeKey;
  auto validPredicate = [&login, &workerId](const StatsRecord &record) -> bool {
    return record.Login == login && record.WorkerId == workerId;
  };

  {
    StatsRecord record;
    record.Login = login;
    record.WorkerId = workerId;
    record.Time.TimeEnd = Timestamp(std::numeric_limits<int64_t>::max());
    record.serializeKey(resumeKey);
  }

  {
    StatsRecord keyRecord;
    keyRecord.Login = login;
    keyRecord.WorkerId = workerId;
    keyRecord.Time.TimeEnd = Timestamp::fromUnixTime(timeTo);
    It->seekForPrev<StatsRecord>(keyRecord, resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  // Fill 'stats' with zero-initialized elements for entire range
  int64_t firstTimeLabel = 0;
  std::vector<CStatsElement> stats;

  {
    firstTimeLabel = (timeFrom+1) + groupByInterval - ((timeFrom+1) % groupByInterval);
    int64_t lastTimeLabel = timeTo + groupByInterval - (timeTo % groupByInterval);
    size_t count = (lastTimeLabel - firstTimeLabel) / groupByInterval + 1;
    if (count > 3200) {
      LOG_F(WARNING, "statisticDb: too much count %zu", count);
      return;
    }

    stats.resize(count);

    int64_t timeLabel = firstTimeLabel;
    for (size_t i = 0; i < count; i++) {
      stats[i].Time.TimeEnd = Timestamp::fromUnixTime(timeLabel);
      timeLabel += groupByInterval;
    }
  }

  while (It->valid()) {
    int64_t recordTimeSeconds = valueRecord.Time.TimeEnd.toUnixTime();
    if (recordTimeSeconds <= timeFrom)
      break;

    if (isDebugStatistic())
      LOG_F(1,
            "getHistory: use row with time=%" PRIi64 " shares=%" PRIu64 " work=%s",
            recordTimeSeconds,
            valueRecord.ShareCount,
            valueRecord.ShareWork.getDecimal().c_str());

    int64_t alignedTimeLabel = recordTimeSeconds + groupByInterval - (recordTimeSeconds % groupByInterval);
    size_t index = (alignedTimeLabel - firstTimeLabel) / groupByInterval;
    if (index < stats.size()) {
      CStatsElement &current = stats[index];
      current.SharesNum += valueRecord.ShareCount;
      current.SharesWork += valueRecord.ShareWork;
      current.PrimePOWTarget = std::min(current.PrimePOWTarget, valueRecord.PrimePOWTarget);
      if (current.PrimePOWSharesNum.size() < valueRecord.PrimePOWShareCount.size())
        current.PrimePOWSharesNum.resize(valueRecord.PrimePOWShareCount.size());
      for (size_t i = 0, ie = valueRecord.PrimePOWShareCount.size(); i != ie; ++i)
        current.PrimePOWSharesNum[i] += valueRecord.PrimePOWShareCount[i];
    }

    It->prev<StatsRecord>(resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  history.resize(stats.size());
  for (size_t i = 0, ie = stats.size(); i != ie; ++i) {
    history[i].Time = stats[i].Time.TimeEnd;
    history[i].SharesPerSecond = static_cast<double>(stats[i].SharesNum) / groupByInterval;
    history[i].AveragePower = CoinInfo_.calculateAveragePower(stats[i].SharesWork, groupByInterval, stats[i].PrimePOWTarget);
    history[i].SharesWork = stats[i].SharesWork;
  }
}

void StatisticDb::exportRecentStats(std::vector<CStatsExportData> &result)
{
  UserStats_.exportRecentStats(AccumulationBegin_, result);
}

void StatisticDb::queryPoolStatsImpl(QueryPoolStatsCallback callback)
{
  callback(getPoolStats());
}

void StatisticDb::queryUserStatsImpl(const std::string &user, QueryUserStatsCallback callback, size_t offset, size_t size, StatisticDb::EStatsColumn sortBy, bool sortDescending)
{
  CStats aggregate;
  std::vector<CStats> workers;
  getUserStats(user, aggregate, workers, offset, size, sortBy, sortDescending);
  callback(aggregate, workers);
}

void StatisticDb::queryAllUserStatsImpl(const std::vector<UserManager::Credentials> &users,
                                        QueryAllUsersStatisticCallback callback,
                                        size_t offset,
                                        size_t size,
                                        CredentialsWithStatistic::EColumns sortBy,
                                        bool sortDescending)
{
  std::vector<CredentialsWithStatistic> usersWithStatistic(users.size());
  for (size_t i = 0, ie = users.size(); i != ie; ++i) {
    const UserManager::Credentials &src = users[i];
    CredentialsWithStatistic &dst = usersWithStatistic[i];
    // TODO: check for std::move
    dst.Credentials = src;

    auto userIt = UserStats_.map().find(makeStatsKey(dst.Credentials.Login, ""));
    if (userIt == UserStats_.map().end())
      continue;

    CStats userStats;
    userIt->second.calcAverageMetrics(CoinInfo_, _cfg.StatisticWorkersPowerCalculateInterval, _cfg.StatisticWorkerGridInterval, userStats);
    dst.WorkersNum = userStats.WorkersNum;
    dst.AveragePower = userStats.AveragePower;
    dst.SharesPerSecond = userStats.SharesPerSecond;
    dst.LastShareTime = userStats.LastShareTime;
  }

  switch (sortBy) {
    case CredentialsWithStatistic::ELogin :
      if (!sortDescending)
        std::sort(usersWithStatistic.begin(), usersWithStatistic.end(), [](const CredentialsWithStatistic &l, const CredentialsWithStatistic &r) { return l.Credentials.Login < r.Credentials.Login; });
      else
        std::sort(usersWithStatistic.rbegin(), usersWithStatistic.rend(), [](const CredentialsWithStatistic &l, const CredentialsWithStatistic &r) { return l.Credentials.Login < r.Credentials.Login; });
      break;
    case CredentialsWithStatistic::EWorkersNum :
      if (!sortDescending)
        std::sort(usersWithStatistic.begin(), usersWithStatistic.end(), [](const CredentialsWithStatistic &l, const CredentialsWithStatistic &r) { return l.WorkersNum < r.WorkersNum; });
      else
        std::sort(usersWithStatistic.rbegin(), usersWithStatistic.rend(), [](const CredentialsWithStatistic &l, const CredentialsWithStatistic &r) { return l.WorkersNum < r.WorkersNum; });
      break;
    case CredentialsWithStatistic::EAveragePower :
      if (!sortDescending)
        std::sort(usersWithStatistic.begin(), usersWithStatistic.end(), [](const CredentialsWithStatistic &l, const CredentialsWithStatistic &r) { return l.AveragePower < r.AveragePower; });
      else
        std::sort(usersWithStatistic.rbegin(), usersWithStatistic.rend(), [](const CredentialsWithStatistic &l, const CredentialsWithStatistic &r) { return l.AveragePower < r.AveragePower; });
      break;
    case CredentialsWithStatistic::ESharesPerSecond :
      if (!sortDescending)
        std::sort(usersWithStatistic.begin(), usersWithStatistic.end(), [](const CredentialsWithStatistic &l, const CredentialsWithStatistic &r) { return l.SharesPerSecond < r.SharesPerSecond; });
      else
        std::sort(usersWithStatistic.rbegin(), usersWithStatistic.rend(), [](const CredentialsWithStatistic &l, const CredentialsWithStatistic &r) { return l.SharesPerSecond < r.SharesPerSecond; });
      break;
    case CredentialsWithStatistic::ELastShareTime :
      if (!sortDescending)
        std::sort(usersWithStatistic.begin(), usersWithStatistic.end(), [](const CredentialsWithStatistic &l, const CredentialsWithStatistic &r) { return l.LastShareTime < r.LastShareTime; });
      else
        std::sort(usersWithStatistic.rbegin(), usersWithStatistic.rend(), [](const CredentialsWithStatistic &l, const CredentialsWithStatistic &r) { return l.LastShareTime < r.LastShareTime; });
      break;
    default:
      break;
  }

  // Make page
  std::vector<CredentialsWithStatistic> result;
  if (offset < usersWithStatistic.size()) {
    size_t endIdx = std::min(offset + size, usersWithStatistic.size());
    result.insert(result.end(), std::make_move_iterator(usersWithStatistic.begin() + offset), std::make_move_iterator(usersWithStatistic.begin() + endIdx));
  }

  callback(result);
}

StatisticServer::StatisticServer(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo) :
  Base_(base), Cfg_(config), CoinInfo_(coinInfo), TaskHandler_(this, base)
{
  Statistics_.reset(new StatisticDb(Base_, config, CoinInfo_));
  StatisticShareLogConfig shareLogConfig(Statistics_.get());
  ShareLog_.init(config.dbPath, coinInfo.Name, Base_, config.ShareLogFlushInterval, config.ShareLogFileSizeLimit, shareLogConfig);
}

void StatisticServer::start()
{
  Thread_ = std::thread([](StatisticServer *server){ server->statisticServerMain(); }, this);
}

void StatisticServer::stop()
{
  Statistics_->stop();
  TaskHandler_.stop(CoinInfo_.Name.c_str(), "StatisticServer task handler");
  postQuitOperation(Base_);
  Thread_.join();
  ShareLog_.flush();
}

void StatisticServer::statisticServerMain()
{
  InitializeWorkerThread();
  loguru::set_thread_name(CoinInfo_.Name.c_str());
  ShareLog_.start();
  TaskHandler_.start();
  Statistics_->start();

  LOG_F(INFO, "<info>: Pool backend for '%s' started, mode is %s, tid=%u", CoinInfo_.Name.c_str(), Cfg_.isMaster ? "MASTER" : "SLAVE", GetGlobalThreadId());
  asyncLoop(Base_);
}

void StatisticServer::onShare(CShare *share)
{
  ShareLog_.addShare(*share);
  Statistics_->addShare(*share);
}
