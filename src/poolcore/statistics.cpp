#include "poolcore/statistics.h"
#include "poolcore/accounting.h"
#include "poolcommon/debug.h"
#include "poolcommon/serialize.h"
#include "loguru.hpp"
#include <algorithm>

inline void StatisticDb::parseStatsCacheFile(CStatsFile &file, std::function<CStatsAccumulator&(const StatsRecord&)> searchAcc)
{
  FileDescriptor fd;
  if (!fd.open(file.Path.u8string().c_str())) {
    LOG_F(ERROR, "StatisticDb: can't open file %s", file.Path.u8string().c_str());
    return;
  }

  size_t fileSize = fd.size();
  xmstream stream(fileSize);
  size_t bytesRead = fd.read(stream.reserve(fileSize), 0, fileSize);
  fd.close();
  if (bytesRead != fileSize) {
    LOG_F(ERROR, "StatisticDb: can't read file %s", file.Path.u8string().c_str());
    return;
  }

  stream.seekSet(0);
  file.LastShareId = stream.readle<uint64_t>();
  while (stream.remaining()) {
    // TODO: use separate format for statistic cache record
    StatsRecord record;
    if (!record.deserializeValue(stream)) {
      LOG_F(ERROR, "StatisticDb: corrupted file %s", file.Path.u8string().c_str());
      break;
    }

    CStatsAccumulator &acc = searchAcc(record);
    if (!acc.Recent.empty() && acc.Recent.back().TimeLabel >= file.TimeLabel) {
      LOG_F(ERROR, "StatisticDb: duplicate data in %s", file.Path.u8string().c_str());
      return;
    }

    acc.Recent.emplace_back();
    CStatsElement &stats = acc.Recent.back();
    acc.LastShareTime = record.Time;
    stats.SharesNum = record.ShareCount;
    stats.SharesWork = record.ShareWork;
    stats.TimeLabel = file.TimeLabel;
    if (isDebugStatistic()) {
      LOG_F(1, "Loaded data from statistic cache: %s/%s shares: %" PRIu64 " work: %.3lf",
            !record.Login.empty() ? record.Login.c_str() : "<empty>",
            !record.WorkerId.empty() ? record.WorkerId.c_str() : "<empty>",
            record.ShareCount,
            record.ShareWork);
    }
  }

  LOG_F(INFO, "Statistic cache file %s loaded successfully", file.Path.u8string().c_str());
}

StatisticDb::StatisticDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo) : Base_(base), _cfg(config), CoinInfo_(coinInfo),
  WorkerStatsDb_(_cfg.dbPath / "workerStats"),
  PoolStatsDb_(_cfg.dbPath / "poolstats")
{
  int64_t currentTime = time(nullptr);
  WorkersFlushInfo_.Time = currentTime;
  WorkersFlushInfo_.ShareId = 0;
  enumerateStatsFiles(WorkersStatsCache_, config.dbPath / "stats.workers.cache");
  for (auto &file: WorkersStatsCache_) {
    parseStatsCacheFile(file, [this](const StatsRecord &record) -> CStatsAccumulator& {
      return !record.WorkerId.empty() ?
        LastWorkerStats_[record.Login][record.WorkerId] :
        LastUserStats_[record.Login];
    });
    WorkersFlushInfo_.Time = file.TimeLabel;
    WorkersFlushInfo_.ShareId = file.LastShareId;
  }

  PoolFlushInfo_.Time = currentTime;
  PoolFlushInfo_.ShareId = 0;
  enumerateStatsFiles(PoolStatsCache_, config.dbPath / "stats.pool.cache");
  for (auto &file: PoolStatsCache_) {
    parseStatsCacheFile(file, [this](const StatsRecord&) -> CStatsAccumulator& { return PoolStatsAcc_; });
    PoolFlushInfo_.Time = file.TimeLabel;
    PoolFlushInfo_.ShareId = file.LastShareId;
  }

  LastKnownShareId_ = std::max(WorkersFlushInfo_.ShareId, PoolFlushInfo_.ShareId);
  if (isDebugStatistic())
    LOG_F(1, "%s: last aggregated id: %" PRIu64 " last known id: %" PRIu64 "", coinInfo.Name.c_str(), lastAggregatedShareId(), lastKnownShareId());
}

void StatisticDb::enumerateStatsFiles(std::deque<CStatsFile> &cache, const std::filesystem::path &directory)
{
  std::error_code errc;
  std::filesystem::create_directories(directory, errc);
  for (std::filesystem::directory_iterator I(directory), IE; I != IE; ++I) {
    std::string fileName = I->path().filename().u8string();
    auto dotDatPos = fileName.find(".dat");
    if (dotDatPos == fileName.npos) {
      LOG_F(ERROR, "StatisticDb: invalid statitic cache file name format: %s", fileName.c_str());
      continue;
    }

    fileName.resize(dotDatPos);

    cache.emplace_back();
    cache.back().Path = *I;
    cache.back().TimeLabel = xatoi<uint64_t>(fileName.c_str());
  }

  std::sort(cache.begin(), cache.end(), [](const CStatsFile &l, const CStatsFile &r){ return l.TimeLabel < r.TimeLabel; });
}

void StatisticDb::updateAcc(const std::string &login, const std::string &workerId, StatisticDb::CStatsAccumulator &acc, time_t currentTime, xmstream &statsFileData)
{
  // Push current accumulated data to ring buffer
  if ((currentTime - acc.LastShareTime) < std::chrono::seconds(_cfg.StatisticKeepWorkerNamesTime).count()) {
    if (isDebugStatistic())
      LOG_F(1, "Update statistics for %s/%s (shares=%u, work=%.3lf)", !login.empty() ? login.c_str() : "<none>", !workerId.empty() ? workerId.c_str() : "<none>", acc.Current.SharesNum, acc.Current.SharesWork);

    // Update in-memory data
    acc.Current.TimeLabel = currentTime;
    acc.Recent.push_back(acc.Current);

    // Update on-disk data
    // Update [user,worker,time] -> state database
    if (acc.Current.SharesNum)
      writeStatsToDb(login, workerId, acc.Current);
    writeStatsToCache(login, workerId, acc.Current, acc.LastShareTime, statsFileData);
  }

  // Reset current worker state
  acc.Current.reset();

  // Remove old data
  auto removeTimePoint = currentTime - std::chrono::seconds(_cfg.StatisticKeepTime).count();
  while (!acc.Recent.empty() && acc.Recent.front().TimeLabel < removeTimePoint)
    acc.Recent.pop_front();
}

void StatisticDb::calcAverageMetrics(const StatisticDb::CStatsAccumulator &acc, std::chrono::seconds calculateInterval, std::chrono::seconds aggregateTime, CStats &result)
{
  // Calculate sum of shares number and work for last N minutes (interval usually defined in config)
  uint32_t workerSharesNum = acc.Current.SharesNum;
  double workerSharesWork = acc.Current.SharesWork;

  int64_t startTimePoint = time(nullptr);
  int64_t stopTimePoint = startTimePoint - calculateInterval.count();
  int64_t lastTimePoint = startTimePoint - aggregateTime.count();
  unsigned counter = 0;
  for (auto statsIt = acc.Recent.rbegin(), statsItEnd = acc.Recent.rend(); statsIt != statsItEnd; ++statsIt) {
    if (statsIt->TimeLabel < stopTimePoint)
      break;

    lastTimePoint = statsIt->TimeLabel - aggregateTime.count();
    workerSharesNum += statsIt->SharesNum;
    workerSharesWork += statsIt->SharesWork;
    counter++;
  }

  uint64_t timeInterval = startTimePoint - lastTimePoint;
  if (isDebugStatistic())
    LOG_F(1, "  * use %u statistic rounds; interval: %" PRIi64 "; shares num: %u; shares work: %.3lf", counter, timeInterval, workerSharesNum, workerSharesWork);

  result.SharesPerSecond = (double)workerSharesNum / timeInterval;
  result.AveragePower = CoinInfo_.calculateAveragePower(workerSharesWork, timeInterval);
  result.SharesWork = workerSharesWork;
}

void StatisticDb::writeStatsToDb(const std::string &loginId, const std::string &workerId, const CStatsElement &element)
{
  StatsRecord record;
  record.Login = loginId;
  record.WorkerId = workerId;
  // record.Time is a record creation time
  record.Time = element.TimeLabel;
  record.ShareCount = element.SharesNum;
  record.ShareWork = element.SharesWork;
  if (!loginId.empty())
    WorkerStatsDb_.put(record);
  else
    PoolStatsDb_.put(record);
}

void StatisticDb::writeStatsToCache(const std::string &loginId, const std::string &workerId, const CStatsElement &element, int64_t lastShareTime, xmstream &statsFileData)
{
  // TODO: use separate format for statistic cache record
  StatsRecord record;
  record.Login = loginId;
  record.WorkerId = workerId;
  // record.Time is a last share time
  // record creation time already have in statistic cache file
  record.Time = lastShareTime;
  record.ShareCount = element.SharesNum;
  record.ShareWork = element.SharesWork;
  record.serializeValue(statsFileData);
}

void StatisticDb::addShare(const CShare &share)
{
  // Update worker stats
  auto &acc = LastWorkerStats_[share.userId][share.workerId];
  acc.Current.SharesNum++;
  acc.Current.SharesWork += share.WorkValue;
  acc.LastShareTime = share.Time;
  // Update user stats
  auto &userAcc = LastUserStats_[share.userId];
  userAcc.Current.SharesNum++;
  userAcc.Current.SharesWork += share.WorkValue;
  userAcc.LastShareTime = share.Time;
  // Update pool stats
  PoolStatsAcc_.Current.SharesNum++;
  PoolStatsAcc_.Current.SharesWork += share.WorkValue;
  PoolStatsCached_.LastShareTime = PoolStatsAcc_.LastShareTime = share.Time;
  LastKnownShareId_ = std::max(LastKnownShareId_, share.UniqueShareId);
}

void StatisticDb::replayShare(const CShare &share)
{
  if (share.UniqueShareId > WorkersFlushInfo_.ShareId) {
    auto &acc = LastWorkerStats_[share.userId][share.workerId];
    acc.Current.SharesNum++;
    acc.Current.SharesWork += share.WorkValue;
    acc.LastShareTime = share.Time;

    auto &userAcc = LastUserStats_[share.userId];
    userAcc.Current.SharesNum++;
    userAcc.Current.SharesWork += share.WorkValue;
    userAcc.LastShareTime = share.Time;
  }

  if (share.UniqueShareId > PoolFlushInfo_.ShareId) {
    PoolStatsAcc_.Current.SharesNum++;
    PoolStatsAcc_.Current.SharesWork += share.WorkValue;
    PoolStatsCached_.LastShareTime = PoolStatsAcc_.LastShareTime = share.Time;
  }

  LastKnownShareId_ = std::max(LastKnownShareId_, share.UniqueShareId);
  if (isDebugStatistic()) {
    Dbg_.MinShareId = std::min(Dbg_.MinShareId, share.UniqueShareId);
    Dbg_.MaxShareId = std::max(Dbg_.MaxShareId, share.UniqueShareId);
    if (share.UniqueShareId > WorkersFlushInfo_.ShareId || share.UniqueShareId > PoolFlushInfo_.ShareId)
      Dbg_.Count++;
  }
}

void StatisticDb::initializationFinish(int64_t timeLabel)
{
  if (isDebugStatistic()) {
    LOG_F(1, "initializationFinish: timeLabel: %" PRIu64 "", timeLabel);
    LOG_F(1, " * workers interval: %" PRIi64 " diff: %" PRIi64"",
          std::chrono::seconds(_cfg.StatisticWorkersAggregateTime).count(),
          timeLabel - WorkersFlushInfo_.Time);
    LOG_F(1, " * pool interval: %" PRIi64 " diff: %" PRIi64"",
          std::chrono::seconds(_cfg.StatisticPoolAggregateTime).count(),
          timeLabel - PoolFlushInfo_.Time);
  }

  if (timeLabel >= WorkersFlushInfo_.Time + std::chrono::seconds(_cfg.StatisticWorkersAggregateTime).count())
    updateWorkersStats(WorkersFlushInfo_.Time + _cfg.StatisticWorkersAggregateTime.count());
  if (timeLabel >= PoolFlushInfo_.Time + std::chrono::seconds(_cfg.StatisticPoolAggregateTime).count())
    updatePoolStats(PoolFlushInfo_.Time + std::chrono::seconds(_cfg.StatisticPoolAggregateTime).count());

  if (isDebugStatistic()) {
    LOG_F(1, "%s: replayed %" PRIu64 " shares from %" PRIu64 " to %" PRIu64 "", CoinInfo_.Name.c_str(), Dbg_.Count, Dbg_.MinShareId, Dbg_.MaxShareId);
  }
}

void StatisticDb::start()
{
  coroutineCall(coroutineNew([](void *arg) {
    StatisticDb *db = static_cast<StatisticDb*>(arg);
    aioUserEvent *timerEvent = newUserEvent(db->Base_, 0, nullptr, nullptr);
    for (;;) {
      ioSleep(timerEvent, std::chrono::microseconds(db->_cfg.StatisticWorkersAggregateTime).count());
      db->updateWorkersStats(time(nullptr));
    }
  }, this, 0x20000));

  coroutineCall(coroutineNew([](void *arg) {
    StatisticDb *db = static_cast<StatisticDb*>(arg);
    aioUserEvent *timerEvent = newUserEvent(db->Base_, 0, nullptr, nullptr);
    for (;;) {
      ioSleep(timerEvent, std::chrono::microseconds(db->_cfg.StatisticPoolAggregateTime).count());
      db->updatePoolStats(time(nullptr));
    }
  }, this, 0x20000));
}

void StatisticDb::updateWorkersStats(int64_t timeLabel)
{
  xmstream statsFileData;
  std::vector<std::string> userDeleteList;
  for (auto &userIt: LastWorkerStats_) {
    std::vector<std::string> workerDeleteList;
    for (auto &workerIt: userIt.second) {
      CStatsAccumulator &acc = workerIt.second;
      updateAcc(userIt.first, workerIt.first, acc, timeLabel, statsFileData);
      if (acc.Recent.empty())
        workerDeleteList.push_back(workerIt.first);
    }

    // Cleanup workers table
    std::for_each(workerDeleteList.begin(), workerDeleteList.end(), [&userIt](const std::string &name) { userIt.second.erase(name);});
  }

  for (auto &userIt: LastUserStats_) {
    CStatsAccumulator &acc = userIt.second;
    updateAcc(userIt.first, "", acc, timeLabel, statsFileData);
    if (!acc.Recent.empty())
      userDeleteList.push_back(userIt.first);
  }

  updateWorkersStatsDiskCache(timeLabel, LastKnownShareId_, statsFileData.data(), statsFileData.sizeOf());

  // Cleanup users table
  std::for_each(userDeleteList.begin(), userDeleteList.end(), [this](const std::string &name) { LastWorkerStats_.erase(name);});
}

void StatisticDb::updatePoolStats(int64_t timeLabel)
{
  PoolStatsCached_.ClientsNum = 0;
  PoolStatsCached_.WorkersNum = 0;

  for (auto &userIt: LastWorkerStats_) {
    bool isActiveUser = false;
    for (auto &workerIt: userIt.second) {
      if ((timeLabel - workerIt.second.LastShareTime) < std::chrono::seconds(_cfg.StatisticWorkersPowerCalculateInterval).count()) {
        PoolStatsCached_.WorkersNum++;
        isActiveUser = true;
      }
    }

    if (isActiveUser)
      PoolStatsCached_.ClientsNum++;
  }

  // Update pool accumulated data
  // Calculate pool power and share rate
  if (isDebugStatistic())
    LOG_F(1, "update pool stats:");
  calcAverageMetrics(PoolStatsAcc_, _cfg.StatisticPoolPowerCalculateInterval, _cfg.StatisticPoolAggregateTime, PoolStatsCached_);

  xmstream statsFileData;
  updateAcc("", "", PoolStatsAcc_, timeLabel, statsFileData);
  updatePoolStatsDiskCache(timeLabel, LastKnownShareId_, statsFileData.data(), statsFileData.sizeOf());

  LOG_F(INFO,
        "clients: %u, workers: %u, power: %" PRIu64 ", share rate: %.3lf shares/s",
        PoolStatsCached_.ClientsNum,
        PoolStatsCached_.WorkersNum,
        PoolStatsCached_.AveragePower,
        PoolStatsCached_.SharesPerSecond);
}

void StatisticDb::updateStatsDiskCache(const char *name, std::deque<CStatsFile> &cache, uint64_t timeLabel, uint64_t lastShareId, const void *data, size_t size)
{
  // Don't write empty files to disk
  if (!size)
    return;

  CStatsFile &statsFile = cache.emplace_back();
  statsFile.Path = _cfg.dbPath / name / (std::to_string(timeLabel)+".dat");
  statsFile.LastShareId = lastShareId;
  statsFile.TimeLabel = timeLabel;

  FileDescriptor fd;
  if (!fd.open(statsFile.Path)) {
    LOG_F(ERROR, "StatisticDb: can't write file %s", statsFile.Path.u8string().c_str());
    return;
  }

  {
    uint64_t serializedShareId = xhtole(lastShareId);
    fd.write(&serializedShareId, sizeof(serializedShareId));
  }

  fd.write(data, size);
  fd.close();

  auto removeTimePoint = timeLabel - std::chrono::seconds(_cfg.StatisticKeepTime).count();
  while (!cache.empty() && cache.front().TimeLabel < removeTimePoint) {
    if (isDebugStatistic())
      LOG_F(1, "Removing old statistic cache file %s", cache.front().Path.u8string().c_str());
    std::filesystem::remove(cache.front().Path);
    cache.pop_front();
  }
}

void StatisticDb::getUserStats(const std::string &user, CStats &userStats, std::vector<CStats> &workerStats, size_t offset, size_t size, EStatsColumn sortBy, bool sortDescending)
{
  auto userIt = LastWorkerStats_.find(user);
  if (userIt == LastWorkerStats_.end())
    return;

  userStats.ClientsNum = 1;
  userStats.WorkersNum = 0;

  // Iterate over all workers
  std::vector<CStats> allStats;
  allStats.resize(userIt->second.size());
  size_t workerStatsIndex = 0;
  int64_t lastShareTime = 0;
  for (const auto &workerIt: userIt->second) {
    const CStatsAccumulator &acc = workerIt.second;
    CStats &result = allStats[workerStatsIndex];
    result.WorkerId = workerIt.first;
    if (isDebugStatistic())
      LOG_F(1, "Retrieve statistic for %s/%s", user.c_str(), workerIt.first.c_str());
    calcAverageMetrics(acc, _cfg.StatisticWorkersPowerCalculateInterval, _cfg.StatisticWorkersAggregateTime, result);

    userStats.SharesPerSecond += result.SharesPerSecond;
    userStats.SharesWork += result.SharesWork;
    userStats.AveragePower += result.AveragePower;

    if (result.AveragePower)
      userStats.WorkersNum++;
    result.LastShareTime = acc.LastShareTime;
    lastShareTime = std::max(lastShareTime, acc.LastShareTime);
    workerStatsIndex++;
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
    LOG_F(1, "getHistory for %s/%s from %" PRIi64 " to % " PRIi64 " group interval %" PRIi64 "", login.c_str(), workerId.c_str(), timeFrom, timeTo, groupByInterval);
  auto &db = !login.empty() ? WorkerStatsDb_ : PoolStatsDb_;
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());

  xmstream resumeKey;
  {
    StatsRecord record;
    record.Login = login;
    record.WorkerId = workerId;
    record.Time = std::numeric_limits<int64_t>::max();
    record.serializeKey(resumeKey);
  }

  {
    StatsRecord record;
    record.Login = login;
    record.WorkerId = workerId;
    record.Time = timeTo;
    It->seekForPrev(record);
  }

  auto endPredicate = [&login, &workerId](const void *key, size_t size) -> bool {
    StatsRecord record;
    xmstream stream(const_cast<void*>(key), size);
    if (!record.deserializeValue(stream)) {
      LOG_F(ERROR, "Statistic database corrupt!");
      return true;
    }

    return record.Login != login || record.WorkerId != workerId;
  };

  std::vector<CStatsElement> stats;
  while (It->valid()) {
    StatsRecord record;
    RawData data = It->value();
    if (!record.deserializeValue(data.data, data.size)) {
      LOG_F(ERROR, "Statistic database corrupt!");
      break;
    }

    if (record.Login != login || record.WorkerId != workerId) {
      It->prev(endPredicate, resumeKey.data(), resumeKey.sizeOf());
      continue;
    }

    if (record.Time <= timeFrom)
      break;

    if (isDebugStatistic())
      LOG_F(1, "getHistory: use row with time=%" PRIi64 " shares=%" PRIu64 " work=%.3lf", record.Time, record.ShareCount, record.ShareWork);

    if (stats.empty() || record.Time < (stats.back().TimeLabel - groupByInterval)) {
      stats.emplace_back();
      CStatsElement &current = stats.back();
      current.TimeLabel = record.Time + groupByInterval - (record.Time % groupByInterval);
      current.SharesNum = record.ShareCount;
      current.SharesWork = record.ShareWork;
    } else {
      CStatsElement &current = stats.back();
      current.SharesNum += record.ShareCount;
      current.SharesWork += record.ShareWork;
    }

    It->prev(endPredicate, resumeKey.data(), resumeKey.sizeOf());
  }

  int64_t lastTimeLabel = 0;
  for (auto It = stats.rbegin(), ItE = stats.rend(); It != ItE; ++It) {
    int64_t expectedTime = lastTimeLabel + groupByInterval;
    if (lastTimeLabel != 0) {
      while (expectedTime < It->TimeLabel) {
        CStats &stats = history.emplace_back();
        stats.Time = expectedTime;
        expectedTime += groupByInterval;
      }
    }

    CStats &stats = history.emplace_back();
    stats.Time = It->TimeLabel;
    stats.SharesPerSecond = (double)It->SharesNum / groupByInterval;
    stats.AveragePower = CoinInfo_.calculateAveragePower(It->SharesWork, groupByInterval);
    stats.SharesWork = It->SharesWork;
    lastTimeLabel = It->TimeLabel;
  }
}

void StatisticDb::exportRecentStats(std::vector<CStatsExportData> &result)
{
  result.clear();
  int64_t timeLabel = time(0);
  for (const auto &userIt: LastUserStats_) {
    auto &userRecord = result.emplace_back();
    userRecord.UserId = userIt.first;
    // Current share work
    {
      auto &current = userRecord.Recent.emplace_back(userIt.second.Current);
      current.TimeLabel = timeLabel;
    }

    // Recent share work (up to N minutes)
    int64_t lastAcceptTime = timeLabel - 30*60;
    for (auto It = userIt.second.Recent.rbegin(), ItE = userIt.second.Recent.rend(); It != ItE; ++It) {
      if (It->TimeLabel < lastAcceptTime)
        break;
      userRecord.Recent.emplace_back(*It);
    }
  }

  std::sort(result.begin(), result.end(), [](const CStatsExportData &l, const CStatsExportData &r) { return l.UserId < r.UserId; });
}
