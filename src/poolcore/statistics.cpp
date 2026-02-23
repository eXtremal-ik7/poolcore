#include "poolcore/statistics.h"
#include "poolcore/statsMergeOperator.h"
#include "poolcommon/debug.h"
#include "loguru.hpp"
#include <algorithm>

StatisticDb::StatisticDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo) : _cfg(config), CoinInfo_(coinInfo),
  PoolStatsAcc_("stats.pool.cache", config.StatisticPoolGridInterval, config.StatisticKeepTime),
  UserStats_("stats.users.cache", config.StatisticUserGridInterval, config.StatisticKeepTime),
  WorkerStats_("stats.workers.cache", config.StatisticWorkerGridInterval, config.StatisticKeepTime),
  StatsDb_(_cfg.dbPath / "statistic", std::make_shared<CWorkSummaryMergeOperator>()),
  ShareLog_(_cfg.dbPath / "statistic.worklog", coinInfo.Name, _cfg.ShareLogFileSizeLimit),
  TaskHandler_(this, base),
  PoolFlushTimer_(base),
  UserFlushTimer_(base),
  WorkerFlushTimer_(base),
  ShareLogFlushTimer_(base)
{

  WorkerStats_.load(_cfg.dbPath, coinInfo.Name);
  UserStats_.load(_cfg.dbPath, coinInfo.Name);
  PoolStatsAcc_.load(_cfg.dbPath, coinInfo.Name);

  if (isDebugStatistic())
    LOG_F(1, "%s: last aggregated id: %" PRIu64 " last known id: %" PRIu64 "", coinInfo.Name.c_str(), lastAggregatedShareId(), lastKnownShareId());

  ShareLog_.replay([this](uint64_t messageId, const CWorkSummaryBatch &batch) {
    WorkerStats_.addBatch(messageId, batch);
    UserStats_.addBatch(messageId, batch, false);
    PoolStatsAcc_.addBatch(messageId, batch);
    if (!batch.Entries.empty())
      PoolStatsCached_.LastShareTime = batch.Time.TimeEnd;
    if (isDebugStatistic()) {
      Dbg_.MinShareId = std::min(Dbg_.MinShareId, messageId);
      Dbg_.MaxShareId = std::max(Dbg_.MaxShareId, messageId);
      Dbg_.Count++;
    }
  });

  if (isDebugStatistic())
    LOG_F(1, "%s: replayed %" PRIu64 " shares from %" PRIu64 " to %" PRIu64 "", coinInfo.Name.c_str(), Dbg_.Count, Dbg_.MinShareId, Dbg_.MaxShareId);

  // Flush replayed data immediately so AccumulationInterval_ is reset.
  // Otherwise, if the pool was down for a long time, the first live share
  // would stretch the interval and corrupt grid distribution.
  Timestamp now = Timestamp::now();
  flushPool(now);
  flushUsers(now);
  flushWorkers(now);

  ShareLog_.startLogging(lastKnownShareId() + 1);
}

void StatisticDb::updatePoolStatsCached(Timestamp currentTime)
{
  PoolStatsCached_.ClientsNum = 0;
  PoolStatsCached_.WorkersNum = 0;

  // Uses the same interval as workers: a user with one worker has identical precision,
  // and multiple workers only improve it
  for (const auto &[key, acc]: UserStats_.map()) {
    if (currentTime - acc.LastShareTime < _cfg.StatisticWorkersPowerCalculateInterval)
      PoolStatsCached_.ClientsNum++;
  }

  for (const auto &[key, acc]: WorkerStats_.map()) {
    if (currentTime - acc.LastShareTime < _cfg.StatisticWorkersPowerCalculateInterval)
      PoolStatsCached_.WorkersNum++;
  }

  if (isDebugStatistic())
    LOG_F(1, "update pool stats:");
  PoolStatsAcc_.series().calcAverageMetrics(CoinInfo_, _cfg.StatisticPoolPowerCalculateInterval, currentTime, PoolStatsCached_);

  LOG_F(INFO,
        "clients: %u, workers: %u, power: %" PRIu64 ", share rate: %.3lf shares/s",
        PoolStatsCached_.ClientsNum,
        PoolStatsCached_.WorkersNum,
        PoolStatsCached_.AveragePower,
        PoolStatsCached_.SharesPerSecond);
}

void StatisticDb::flushPool(Timestamp currentTime)
{
  PoolStatsAcc_.flush(currentTime, _cfg.dbPath, &StatsDb_);
  updatePoolStatsCached(currentTime);
}

void StatisticDb::flushUsers(Timestamp currentTime)
{
  UserStats_.flush(currentTime, _cfg.dbPath, &StatsDb_);
}

void StatisticDb::flushWorkers(Timestamp currentTime)
{
  WorkerStats_.flush(currentTime, _cfg.dbPath, &StatsDb_);
}

void StatisticDb::onWorkSummary(const CWorkSummaryBatch &batch)
{
  if (batch.Time.TimeBegin > batch.Time.TimeEnd ||
      (batch.Time.TimeEnd - batch.Time.TimeBegin) > MaxBatchTimeInterval) {
    LOG_F(ERROR,
          "StatisticDb::onWorkSummary: invalid batch time [%" PRId64 ", %" PRId64 "], %zu entries dropped",
          batch.Time.TimeBegin.count(),
          batch.Time.TimeEnd.count(),
          batch.Entries.size());
    return;
  }

  uint64_t messageId = ShareLog_.addMessage(batch);
  WorkerStats_.addBatch(messageId, batch);
  UserStats_.addBatch(messageId, batch, false);
  PoolStatsAcc_.addBatch(messageId, batch);
  if (!batch.Entries.empty())
    PoolStatsCached_.LastShareTime = batch.Time.TimeEnd;
}


void StatisticDb::start()
{
  TaskHandler_.start();

  ShareLogFlushTimer_.start([this]() {
    ShareLog_.flush();
    ShareLog_.cleanupOldFiles(lastAggregatedShareId());
  }, _cfg.ShareLogFlushInterval, false, true);

  PoolFlushTimer_.start([this]() {
    flushPool(Timestamp::now());
  }, _cfg.StatisticPoolFlushInterval, false, true);

  UserFlushTimer_.start([this]() {
    flushUsers(Timestamp::now());
  }, _cfg.StatisticUserFlushInterval, false, true);

  WorkerFlushTimer_.start([this]() {
    flushWorkers(Timestamp::now());
  }, _cfg.StatisticWorkerFlushInterval, false, true);
}

void StatisticDb::stop()
{
  const char *coin = CoinInfo_.Name.c_str();
  TaskHandler_.stop(coin, "statisticDb task handler");
  PoolFlushTimer_.stop();
  UserFlushTimer_.stop();
  WorkerFlushTimer_.stop();
  ShareLogFlushTimer_.stop();
  PoolFlushTimer_.wait(coin, "statisticDb pool flush");
  UserFlushTimer_.wait(coin, "statisticDb user flush");
  WorkerFlushTimer_.wait(coin, "statisticDb worker flush");
  ShareLogFlushTimer_.wait(coin, "statisticDb share log flush");
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

  CWorkSummaryEntryWithTime valueRecord;
  xmstream resumeKey;
  auto validPredicate = [&login, &workerId](const CWorkSummaryEntryWithTime &record) -> bool {
    return record.UserId == login && record.WorkerId == workerId;
  };

  {
    CWorkSummaryEntryWithTime record;
    record.UserId = login;
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
    CWorkSummaryEntryWithTime keyRecord;
    keyRecord.UserId = login;
    keyRecord.WorkerId = workerId;
    keyRecord.Time.TimeEnd = gridStart;
    It->seek<CWorkSummaryEntryWithTime>(keyRecord, resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
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
            valueRecord.Data.SharesNum,
            valueRecord.Data.SharesWork.getDecimal().c_str());

    Timestamp cellBegin = gridStart + groupBy * static_cast<int64_t>(firstIdx);
    Timestamp cellEnd = cellBegin + groupBy;
    for (size_t i = firstIdx; i < lastIdx; i++) {
      double fraction = overlapFraction(recordBegin, recordEnd, cellBegin, cellEnd);
      if (fraction >= 1.0)
        history[i].merge(valueRecord.Data);
      else
        history[i].mergeScaled(valueRecord.Data, fraction);

      cellBegin = cellEnd;
      cellEnd += groupBy;
    }

    It->next<CWorkSummaryEntryWithTime>(resumeKey.data<const char>(), resumeKey.sizeOf(), valueRecord, validPredicate);
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

void StatisticServer::onWorkSummary(const CWorkSummaryBatch &batch)
{
  Statistics_->onWorkSummary(batch);
}
