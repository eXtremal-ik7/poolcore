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
  PoolFlushEvent_ = newUserEvent(base, 1, nullptr, nullptr);
  UserFlushEvent_ = newUserEvent(base, 1, nullptr, nullptr);
  WorkerFlushEvent_ = newUserEvent(base, 1, nullptr, nullptr);

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
    if (timeLabel - acc.LastShareTime < _cfg.StatisticWorkersPowerCalculateInterval)
      PoolStatsCached_.ClientsNum++;
  }

  for (const auto &[key, acc]: WorkerStats_.map()) {
    if (timeLabel - acc.LastShareTime < _cfg.StatisticWorkersPowerCalculateInterval)
      PoolStatsCached_.WorkersNum++;
  }

  if (isDebugStatistic())
    LOG_F(1, "update pool stats:");
  PoolStatsAcc_.series().calcAverageMetrics(CoinInfo_, _cfg.StatisticPoolPowerCalculateInterval, PoolStatsCached_);

  LOG_F(INFO,
        "clients: %u, workers: %u, power: %" PRIu64 ", share rate: %.3lf shares/s",
        PoolStatsCached_.ClientsNum,
        PoolStatsCached_.WorkersNum,
        PoolStatsCached_.AveragePower,
        PoolStatsCached_.SharesPerSecond);
}

void StatisticDb::flushPool(Timestamp timeLabel)
{
  int64_t gridMs = std::chrono::duration_cast<std::chrono::milliseconds>(_cfg.StatisticPoolGridInterval).count();
  Timestamp removeTimePoint = timeLabel - _cfg.StatisticKeepTime;

  kvdb<rocksdbBase>::Batch batch;
  std::set<int64_t> modifiedTimes, removedTimes;
  PoolStatsAcc_.flush(timeLabel, gridMs, removeTimePoint, LastKnownShareId_, &batch, modifiedTimes, removedTimes);
  StatsDb_.writeBatch(batch);

  for (int64_t t : modifiedTimes)
    PoolStatsAcc_.rebuildDatFile(_cfg.dbPath, t);
  for (int64_t t : removedTimes)
    removeDatFile(_cfg.dbPath, PoolStatsAcc_.cachePath(), t);

  updatePoolStatsCached(timeLabel);
}

void StatisticDb::flushUsers(Timestamp timeLabel)
{
  int64_t gridMs = std::chrono::duration_cast<std::chrono::milliseconds>(_cfg.StatisticUserGridInterval).count();
  Timestamp removeTimePoint = timeLabel - _cfg.StatisticKeepTime;

  kvdb<rocksdbBase>::Batch batch;
  std::set<int64_t> modifiedTimes, removedTimes;
  UserStats_.flush(timeLabel, gridMs, removeTimePoint, LastKnownShareId_, &batch, modifiedTimes, removedTimes);
  StatsDb_.writeBatch(batch);

  for (int64_t t : modifiedTimes)
    UserStats_.rebuildDatFile(_cfg.dbPath, t);
  for (int64_t t : removedTimes)
    removeDatFile(_cfg.dbPath, UserStats_.cachePath(), t);
}

void StatisticDb::flushWorkers(Timestamp timeLabel)
{
  int64_t gridMs = std::chrono::duration_cast<std::chrono::milliseconds>(_cfg.StatisticWorkerGridInterval).count();
  Timestamp removeTimePoint = timeLabel - _cfg.StatisticKeepTime;

  kvdb<rocksdbBase>::Batch batch;
  std::set<int64_t> modifiedTimes, removedTimes;
  WorkerStats_.flush(timeLabel, gridMs, removeTimePoint, LastKnownShareId_, &batch, modifiedTimes, removedTimes);
  StatsDb_.writeBatch(batch);

  for (int64_t t : modifiedTimes)
    WorkerStats_.rebuildDatFile(_cfg.dbPath, t);
  for (int64_t t : removedTimes)
    removeDatFile(_cfg.dbPath, WorkerStats_.cachePath(), t);
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
  PoolStatsAcc_.setAccumulationBegin(timeLabel);
  UserStats_.setAccumulationBegin(timeLabel);
  WorkerStats_.setAccumulationBegin(timeLabel);

  if (isDebugStatistic())
    LOG_F(1, "%s: replayed %" PRIu64 " shares from %" PRIu64 " to %" PRIu64 "", CoinInfo_.Name.c_str(), Dbg_.Count, Dbg_.MinShareId, Dbg_.MaxShareId);
}

void StatisticDb::start()
{
  TaskHandler_.start();

  coroutineCall(coroutineNewWithCb([](void *arg) {
    StatisticDb *db = static_cast<StatisticDb*>(arg);
    for (;;) {
      ioSleep(db->PoolFlushEvent_, std::chrono::microseconds(db->_cfg.StatisticPoolFlushInterval).count());
      db->flushPool(Timestamp::now());
      if (db->ShutdownRequested_)
        break;
    }
  }, this, 0x20000, coroutineFinishCb, &PoolFlushFinished_));

  coroutineCall(coroutineNewWithCb([](void *arg) {
    StatisticDb *db = static_cast<StatisticDb*>(arg);
    for (;;) {
      ioSleep(db->UserFlushEvent_, std::chrono::microseconds(db->_cfg.StatisticUserFlushInterval).count());
      db->flushUsers(Timestamp::now());
      if (db->ShutdownRequested_)
        break;
    }
  }, this, 0x20000, coroutineFinishCb, &UserFlushFinished_));

  coroutineCall(coroutineNewWithCb([](void *arg) {
    StatisticDb *db = static_cast<StatisticDb*>(arg);
    for (;;) {
      ioSleep(db->WorkerFlushEvent_, std::chrono::microseconds(db->_cfg.StatisticWorkerFlushInterval).count());
      db->flushWorkers(Timestamp::now());
      if (db->ShutdownRequested_)
        break;
    }
  }, this, 0x20000, coroutineFinishCb, &WorkerFlushFinished_));
}

void StatisticDb::stop()
{
  ShutdownRequested_ = true;
  userEventActivate(PoolFlushEvent_);
  userEventActivate(UserFlushEvent_);
  userEventActivate(WorkerFlushEvent_);
  TaskHandler_.stop(CoinInfo_.Name.c_str(), "statisticDb task handler");
  coroutineJoin(CoinInfo_.Name.c_str(), "statisticDb pool flush", &PoolFlushFinished_);
  coroutineJoin(CoinInfo_.Name.c_str(), "statisticDb user flush", &UserFlushFinished_);
  coroutineJoin(CoinInfo_.Name.c_str(), "statisticDb worker flush", &WorkerFlushFinished_);
}


void StatisticDb::getUserStats(const std::string &user, CStats &userStats, std::vector<CStats> &workerStats, size_t offset, size_t size, EStatsColumn sortBy, bool sortDescending)
{
  auto workerLo = WorkerStats_.map().lower_bound(makeStatsKey(user, ""));
  if (workerLo == WorkerStats_.map().end() || splitStatsKey(workerLo->first).first != user)
    return;

  userStats.ClientsNum = 1;
  userStats.WorkersNum = 0;

  std::vector<CStats> allStats;
  Timestamp lastShareTime;

  for (auto it = workerLo; it != WorkerStats_.map().end(); ++it) {
    auto [login, workerId] = splitStatsKey(it->first);
    if (login != user)
      break;
    const CStatsSeries &acc = it->second;
    CStats &result = allStats.emplace_back();
    result.WorkerId = std::move(workerId);
    if (isDebugStatistic())
      LOG_F(1, "Retrieve statistic for %s/%s", user.c_str(), result.WorkerId.c_str());
    acc.calcAverageMetrics(CoinInfo_, _cfg.StatisticWorkersPowerCalculateInterval, result);
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
  if (groupByInterval < 60 || timeTo <= timeFrom)
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

  auto groupBy = std::chrono::seconds(groupByInterval);
  Timestamp tsFrom = Timestamp::fromUnixTime(timeFrom);
  Timestamp tsTo = Timestamp::fromUnixTime(timeTo);

  {
    StatsRecord keyRecord;
    keyRecord.Login = login;
    keyRecord.WorkerId = workerId;
    keyRecord.Time.TimeEnd = tsTo;
    It->seekForPrev<StatsRecord>(keyRecord, resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  Timestamp firstTimeLabel = tsFrom.alignUp(groupBy);
  if (firstTimeLabel == tsFrom)
    firstTimeLabel += groupBy;
  Timestamp lastTimeLabel = tsTo.alignUp(groupBy);
  size_t count = (lastTimeLabel - firstTimeLabel) / groupBy + 1;
  if (count > 3200) {
    LOG_F(WARNING, "statisticDb: too much count %zu", count);
    return;
  }

  std::vector<CStatsElement> stats(count);
  int64_t groupByMs = std::chrono::duration_cast<std::chrono::milliseconds>(groupBy).count();

  while (It->valid()) {
    if (valueRecord.Time.TimeEnd <= tsFrom)
      break;

    if (isDebugStatistic())
      LOG_F(1,
            "getHistory: use row with time=%" PRIi64 " shares=%" PRIu64 " work=%s",
            valueRecord.Time.TimeEnd.toUnixTime(),
            valueRecord.ShareCount,
            valueRecord.ShareWork.getDecimal().c_str());

    Timestamp cellEnd = valueRecord.Time.TimeEnd.alignUp(groupBy);

    if (valueRecord.Time.TimeBegin >= cellEnd - groupBy) {
      size_t index = (cellEnd - firstTimeLabel) / groupBy;
      if (index < stats.size())
        stats[index].merge(valueRecord);
    } else {
      CStatsElement element;
      element.SharesNum = valueRecord.ShareCount;
      element.SharesWork = valueRecord.ShareWork;
      element.PrimePOWTarget = valueRecord.PrimePOWTarget;
      element.PrimePOWSharesNum.assign(valueRecord.PrimePOWShareCount.begin(), valueRecord.PrimePOWShareCount.end());

      auto cells = element.distributeToGrid(valueRecord.Time.TimeBegin.count(), valueRecord.Time.TimeEnd.count(), groupByMs);
      for (const auto &cell : cells) {
        size_t index = (cell.Time.TimeEnd - firstTimeLabel) / groupBy;
        if (index < stats.size())
          stats[index].merge(cell);
      }
    }

    It->prev<StatsRecord>(resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  history.resize(count);
  for (size_t i = 0; i < count; i++) {
    history[i].Time = firstTimeLabel + groupBy * static_cast<int64_t>(i);
    history[i].SharesPerSecond = static_cast<double>(stats[i].SharesNum) / groupByInterval;
    history[i].AveragePower = CoinInfo_.calculateAveragePower(stats[i].SharesWork, groupByInterval, stats[i].PrimePOWTarget);
    history[i].SharesWork = stats[i].SharesWork;
  }
}

void StatisticDb::exportRecentStats(std::chrono::seconds keepTime, std::vector<CStatsExportData> &result)
{
  UserStats_.exportRecentStats(keepTime, result);
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
    userIt->second.calcAverageMetrics(CoinInfo_, _cfg.StatisticWorkersPowerCalculateInterval, userStats);
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
