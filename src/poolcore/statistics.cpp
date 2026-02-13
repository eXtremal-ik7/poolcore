#include "poolcore/statistics.h"
#include "poolcore/statsMergeOperator.h"
#include "poolcommon/coroutineJoin.h"
#include "poolcommon/debug.h"
#include "loguru.hpp"
#include <algorithm>

StatisticDb::StatisticDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo) : _cfg(config), CoinInfo_(coinInfo),
  PoolStatsAcc_("stats.pool.cache.3", config.StatisticPoolGridInterval, config.StatisticKeepTime),
  UserStats_("stats.users.cache.3", config.StatisticUserGridInterval, config.StatisticKeepTime),
  WorkerStats_("stats.workers.cache.3", config.StatisticWorkerGridInterval, config.StatisticKeepTime),
  StatsDb_(_cfg.dbPath / "statistic", std::make_shared<StatsRecordMergeOperator>()),
  ShareLog_(_cfg.dbPath / "statistic.worklog", coinInfo.Name, _cfg.ShareLogFileSizeLimit),
  TaskHandler_(this, base)
{
  PoolFlushEvent_ = newUserEvent(base, 1, nullptr, nullptr);
  UserFlushEvent_ = newUserEvent(base, 1, nullptr, nullptr);
  WorkerFlushEvent_ = newUserEvent(base, 1, nullptr, nullptr);
  ShareLogFlushEvent_ = newUserEvent(base, 1, nullptr, nullptr);

  WorkerStats_.load(_cfg.dbPath, coinInfo.Name);
  UserStats_.load(_cfg.dbPath, coinInfo.Name);
  PoolStatsAcc_.load(_cfg.dbPath, coinInfo.Name);

  LastKnownShareId_ = std::max({WorkerStats_.savedShareId(), UserStats_.savedShareId(), PoolStatsAcc_.savedShareId()});
  if (isDebugStatistic())
    LOG_F(1, "%s: last aggregated id: %" PRIu64 " last known id: %" PRIu64 "", coinInfo.Name.c_str(), lastAggregatedShareId(), lastKnownShareId());

  Timestamp initTime = Timestamp::now();
  ShareLog_.replay([this](uint64_t messageId, const std::vector<CWorkSummaryEntry> &entries) {
    replayWorkSummary(messageId, entries);
  });

  PoolStatsAcc_.setAccumulationBegin(initTime);
  UserStats_.setAccumulationBegin(initTime);
  WorkerStats_.setAccumulationBegin(initTime);
  if (isDebugStatistic())
    LOG_F(1, "%s: replayed %" PRIu64 " shares from %" PRIu64 " to %" PRIu64 "", coinInfo.Name.c_str(), Dbg_.Count, Dbg_.MinShareId, Dbg_.MaxShareId);

  ShareLog_.startLogging(lastKnownShareId() + 1);
}

void StatisticDb::updatePoolStatsCached(Timestamp timeLabel)
{
  PoolStatsCached_.ClientsNum = 0;
  PoolStatsCached_.WorkersNum = 0;

  // Uses the same interval as workers: a user with one worker has identical precision,
  // and multiple workers only improve it
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
  PoolStatsAcc_.series().calcAverageMetrics(CoinInfo_, _cfg.StatisticPoolPowerCalculateInterval, timeLabel, PoolStatsCached_);

  LOG_F(INFO,
        "clients: %u, workers: %u, power: %" PRIu64 ", share rate: %.3lf shares/s",
        PoolStatsCached_.ClientsNum,
        PoolStatsCached_.WorkersNum,
        PoolStatsCached_.AveragePower,
        PoolStatsCached_.SharesPerSecond);
}

void StatisticDb::flushPool(Timestamp timeLabel)
{
  PoolStatsAcc_.flush(timeLabel, LastKnownShareId_, _cfg.dbPath, &StatsDb_);
  updatePoolStatsCached(timeLabel);
}

void StatisticDb::flushUsers(Timestamp timeLabel)
{
  UserStats_.flush(timeLabel, LastKnownShareId_, _cfg.dbPath, &StatsDb_);
}

void StatisticDb::flushWorkers(Timestamp timeLabel)
{
  WorkerStats_.flush(timeLabel, LastKnownShareId_, _cfg.dbPath, &StatsDb_);
}

void StatisticDb::onWorkSummary(const std::vector<CWorkSummaryEntry> &entries)
{
  uint64_t messageId = ShareLog_.addShare(entries);
  for (const auto &entry : entries) {
    Timestamp time = entry.Data.Time.TimeEnd;
    WorkerStats_.addWorkSummary(entry.UserId, entry.WorkerId, entry.Data, time);
    UserStats_.addWorkSummary(entry.UserId, "", entry.Data, time);
    PoolStatsAcc_.addWorkSummary(entry.Data, time);
    PoolStatsCached_.LastShareTime = time;
  }
  LastKnownShareId_ = std::max(LastKnownShareId_, messageId);
}

void StatisticDb::replayWorkSummary(uint64_t messageId, const std::vector<CWorkSummaryEntry> &entries)
{
  for (const auto &entry : entries) {
    Timestamp time = entry.Data.Time.TimeEnd;
    if (messageId > WorkerStats_.savedShareId())
      WorkerStats_.addWorkSummary(entry.UserId, entry.WorkerId, entry.Data, time);
    if (messageId > UserStats_.savedShareId())
      UserStats_.addWorkSummary(entry.UserId, "", entry.Data, time);
    if (messageId > PoolStatsAcc_.savedShareId())
      PoolStatsAcc_.addWorkSummary(entry.Data, time);
    PoolStatsCached_.LastShareTime = time;
  }
  LastKnownShareId_ = std::max(LastKnownShareId_, messageId);

  if (isDebugStatistic()) {
    Dbg_.MinShareId = std::min(Dbg_.MinShareId, messageId);
    Dbg_.MaxShareId = std::max(Dbg_.MaxShareId, messageId);
    Dbg_.Count++;
  }
}


void StatisticDb::shareLogFlushHandler()
{
  for (;;) {
    ioSleep(ShareLogFlushEvent_, std::chrono::microseconds(_cfg.ShareLogFlushInterval).count());
    ShareLog_.flush();
    if (ShutdownRequested_)
      break;
    ShareLog_.cleanupOldFiles(lastAggregatedShareId());
  }
}

void StatisticDb::start()
{
  TaskHandler_.start();
  coroutineCall(coroutineNewWithCb([](void *arg) { static_cast<StatisticDb*>(arg)->shareLogFlushHandler(); }, this, 0x100000, coroutineFinishCb, &ShareLogFlushFinished_));

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
  TaskHandler_.stop(CoinInfo_.Name.c_str(), "statisticDb task handler");
  ShutdownRequested_ = true;
  userEventActivate(PoolFlushEvent_);
  userEventActivate(UserFlushEvent_);
  userEventActivate(WorkerFlushEvent_);
  userEventActivate(ShareLogFlushEvent_);
  coroutineJoin(CoinInfo_.Name.c_str(), "statisticDb pool flush", &PoolFlushFinished_);
  coroutineJoin(CoinInfo_.Name.c_str(), "statisticDb user flush", &UserFlushFinished_);
  coroutineJoin(CoinInfo_.Name.c_str(), "statisticDb worker flush", &WorkerFlushFinished_);
  coroutineJoin(CoinInfo_.Name.c_str(), "statisticDb share log flush", &ShareLogFlushFinished_);
  ShareLog_.flush();
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
  Timestamp now = Timestamp::now();

  for (auto it = workerLo; it != WorkerStats_.map().end(); ++it) {
    auto [login, workerId] = splitStatsKey(it->first);
    if (login != user)
      break;
    const CStatsSeries &acc = it->second;
    CStats &result = allStats.emplace_back();
    result.WorkerId = std::move(workerId);
    if (isDebugStatistic())
      LOG_F(1, "Retrieve statistic for %s/%s", user.c_str(), result.WorkerId.c_str());
    acc.calcAverageMetrics(CoinInfo_, _cfg.StatisticWorkersPowerCalculateInterval, now, result);
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
  // TODO: add an error output parameter so the caller can return a JSON error
  //       instead of an empty result indistinguishable from "no data"
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
    record.Time.TimeEnd = Timestamp(0);
    record.serializeKey(resumeKey);
  }

  auto groupBy = std::chrono::seconds(groupByInterval);
  Timestamp tsFrom = Timestamp::fromUnixTime(timeFrom);
  Timestamp tsTo = Timestamp::fromUnixTime(timeTo);

  Timestamp firstTimeLabel = tsFrom.alignUp(groupBy);
  if (firstTimeLabel == tsFrom)
    firstTimeLabel += groupBy;
  Timestamp lastTimeLabel = tsTo.alignUp(groupBy);
  // ~2.2 days at 1-min intervals, ~53 hours at 1-sec intervals
  constexpr size_t MaxHistoryCells = 3200;
  size_t count = (lastTimeLabel - firstTimeLabel) / groupBy + 1;
  if (count > MaxHistoryCells) {
    LOG_F(WARNING, "statisticDb: too much count %zu", count);
    return;
  }

  // Grid layout: cell i covers [gridStart + i*groupBy, gridStart + (i+1)*groupBy)
  // Cell i TimeEnd = firstTimeLabel + i*groupBy
  Timestamp gridStart = firstTimeLabel - groupBy;
  Timestamp gridEnd = gridStart + groupBy * static_cast<int64_t>(count);

  history.resize(count);
  for (size_t i = 0; i < count; i++)
    history[i].Time = firstTimeLabel + groupBy * static_cast<int64_t>(i);

  {
    StatsRecord keyRecord;
    keyRecord.Login = login;
    keyRecord.WorkerId = workerId;
    keyRecord.Time.TimeEnd = gridStart;
    It->seek<StatsRecord>(keyRecord, resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  while (It->valid()) {
    Timestamp recordBegin = valueRecord.Time.TimeBegin;
    Timestamp recordEnd = valueRecord.Time.TimeEnd;

    if (recordBegin >= gridEnd)
      break;

    Timestamp clampedBegin = std::max(recordBegin, gridStart);
    Timestamp clampedEnd = std::min(recordEnd, gridEnd);

    size_t firstIdx = static_cast<size_t>((clampedBegin - gridStart) / groupBy);
    size_t lastIdx = static_cast<size_t>((clampedEnd - gridStart + groupBy - std::chrono::milliseconds(1)) / groupBy);

    if (isDebugStatistic() && firstIdx < lastIdx)
      LOG_F(1,
            "getHistory: use row with time=%" PRIi64 " shares=%" PRIu64 " work=%s",
            valueRecord.Time.TimeEnd.toUnixTime(),
            valueRecord.ShareCount,
            valueRecord.ShareWork.getDecimal().c_str());

    Timestamp cellBegin = gridStart + groupBy * static_cast<int64_t>(firstIdx);
    Timestamp cellEnd = cellBegin + groupBy;
    for (size_t i = firstIdx; i < lastIdx; i++) {
      double fraction = overlapFraction(recordBegin, recordEnd, cellBegin, cellEnd);
      if (fraction >= 1.0)
        history[i].merge(valueRecord);
      else
        history[i].mergeScaled(valueRecord, fraction);

      cellBegin = cellEnd;
      cellEnd += groupBy;
    }

    It->next<StatsRecord>(resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
  }

  // Empty cells keep PrimePOWTarget=-1U (sentinel); calculateAveragePower handles it correctly:
  // EHash ignores it, ECPD returns 0 for -1U
  for (size_t i = 0; i < count; i++) {
    history[i].SharesPerSecond = static_cast<double>(history[i].SharesNum) / groupByInterval;
    history[i].AveragePower = CoinInfo_.calculateAveragePower(history[i].SharesWork, groupByInterval, history[i].PrimePOWTarget);
  }
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
  Timestamp now = Timestamp::now();
  for (size_t i = 0, ie = users.size(); i != ie; ++i) {
    const UserManager::Credentials &src = users[i];
    CredentialsWithStatistic &dst = usersWithStatistic[i];
    // TODO: check for std::move
    dst.Credentials = src;

    auto userIt = UserStats_.map().find(makeStatsKey(dst.Credentials.Login, ""));
    if (userIt == UserStats_.map().end())
      continue;

    CStats userStats;
    userIt->second.calcAverageMetrics(CoinInfo_, _cfg.StatisticWorkersPowerCalculateInterval, now, userStats);
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
}

void StatisticServer::statisticServerMain()
{
  InitializeWorkerThread();
  loguru::set_thread_name(CoinInfo_.Name.c_str());
  TaskHandler_.start();
  Statistics_->start();

  LOG_F(INFO, "<info>: Pool backend for '%s' started, mode is %s, tid=%u", CoinInfo_.Name.c_str(), Cfg_.isMaster ? "MASTER" : "SLAVE", GetGlobalThreadId());
  asyncLoop(Base_);
}

void StatisticServer::onWorkSummary(const std::vector<CWorkSummaryEntry> &entries)
{
  Statistics_->onWorkSummary(entries);
}
