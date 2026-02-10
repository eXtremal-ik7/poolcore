#include "poolcore/statistics.h"
#include "poolcore/accounting.h"
#include "poolcommon/coroutineJoin.h"
#include "poolcommon/debug.h"
#include "poolcommon/path.h"
#include "loguru.hpp"
#include <algorithm>

bool StatisticDb::parseStatsCacheFile(CDatFile &file)

{
  FileDescriptor fd;
  if (!fd.open(path_to_utf8(file.Path).c_str())) {
    LOG_F(ERROR, "StatisticDb: can't open file %s", path_to_utf8(file.Path).c_str());
    return false;
  }

  size_t fileSize = fd.size();
  xmstream stream(fileSize);
  size_t bytesRead = fd.read(stream.reserve(fileSize), 0, fileSize);
  fd.close();
  if (bytesRead != fileSize) {
    LOG_F(ERROR, "StatisticDb: can't read file %s", path_to_utf8(file.Path).c_str());
    return false;
  }

  stream.seekSet(0);

  CStatsFileData fileData;

  DbIo<CStatsFileData>::unserialize(stream, fileData);

  if (!stream.remaining() && !stream.eof()) {
    file.LastShareId = fileData.LastShareId;
    for (const auto &record: fileData.Records) {
      CStatsAccumulator *acc = nullptr;
      if (!record.Login.empty())
        acc = &LastWorkerStats_[makeWorkerStatsKey(record.Login, record.WorkerId)];
      else
        acc = &PoolStatsAcc_;


      if (!acc->Recent.empty() && acc->Recent.back().Time.TimeEnd >= Timestamp::fromUnixTime(file.FileId)) {
        LOG_F(ERROR, "<%s> StatisticDb: duplicate data in %s", CoinInfo_.Name.c_str(), path_to_utf8(file.Path).c_str());
        return false;
      }

      acc->Recent.emplace_back();
      CStatsElement &stats = acc->Recent.back();
      acc->Time.TimeEnd = record.Time.TimeEnd;
      stats.SharesNum = static_cast<uint32_t>(record.ShareCount);
      stats.SharesWork = record.ShareWork;
      stats.Time = record.Time;
      stats.PrimePOWTarget = record.PrimePOWTarget;
      stats.PrimePOWSharesNum.assign(record.PrimePOWShareCount.begin(), record.PrimePOWShareCount.end());
      if (isDebugStatistic()) {
        LOG_F(1, "<%s> Loaded data from statistic cache: %s/%s shares: %" PRIu64 " work: %s",
              CoinInfo_.Name.c_str(),
              !record.Login.empty() ? record.Login.c_str() : "<empty>",
              !record.WorkerId.empty() ? record.WorkerId.c_str() : "<empty>",
              record.ShareCount,
              record.ShareWork.getDecimal().c_str());
      }
    }

    LOG_F(INFO, "<%s> Statistic cache file %s loaded successfully", CoinInfo_.Name.c_str(), path_to_utf8(file.Path).c_str());
    return true;
  } else {
    LOG_F(ERROR, "<%s> StatisticDb: corrupted file %s", CoinInfo_.Name.c_str(), path_to_utf8(file.Path).c_str());
    return false;
  }
}

StatisticDb::StatisticDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo) : _cfg(config), CoinInfo_(coinInfo),
  WorkerStatsDb_(_cfg.dbPath / "workerStats.2"),
  PoolStatsDb_(_cfg.dbPath / "poolstats.2"),
  TaskHandler_(this, base)
{
  WorkerStatsUpdaterEvent_ = newUserEvent(base, 1, nullptr, nullptr);
  PoolStatsUpdaterEvent_ = newUserEvent(base, 1, nullptr, nullptr);

  Timestamp currentTime = Timestamp::now();
  WorkersFlushInfo_.Time = currentTime;
  WorkersFlushInfo_.ShareId = 0;
  std::deque<CDatFile> workerStatsCache;
  enumerateDatFiles(workerStatsCache, config.dbPath / CurrentWorkersStoragePath, 3, true);
  for (auto &file: workerStatsCache) {
    if (parseStatsCacheFile(file)) {
      WorkersFlushInfo_.Time = Timestamp::fromUnixTime(file.FileId);
      WorkersFlushInfo_.ShareId = file.LastShareId;
      WorkersStatsCache_.emplace_back(std::move(file));
    } else {
      std::filesystem::remove(file.Path);
    }
  }

  PoolFlushInfo_.Time = currentTime;
  PoolFlushInfo_.ShareId = 0;
  std::deque<CDatFile> poolStatsCache;
  enumerateDatFiles(poolStatsCache, config.dbPath / CurrentPoolStoragePath, 3, true);
  for (auto &file: poolStatsCache) {
    if (parseStatsCacheFile(file)) {
      PoolFlushInfo_.Time = Timestamp::fromUnixTime(file.FileId);
      PoolFlushInfo_.ShareId = file.LastShareId;
      PoolStatsCache_.emplace_back(std::move(file));
    } else {
      std::filesystem::remove(file.Path);
    }
  }

  LastKnownShareId_ = std::max(WorkersFlushInfo_.ShareId, PoolFlushInfo_.ShareId);
  if (isDebugStatistic())
    LOG_F(1, "%s: last aggregated id: %" PRIu64 " last known id: %" PRIu64 "", coinInfo.Name.c_str(), lastAggregatedShareId(), lastKnownShareId());
}

void StatisticDb::updateAcc(const std::string &login, const std::string &workerId, StatisticDb::CStatsAccumulator &acc, Timestamp currentTime, xmstream &statsFileData)
{
  // Push current accumulated data to ring buffer
  if (currentTime - acc.Time.TimeEnd < std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticKeepWorkerNamesTime)) {
    if (isDebugStatistic())
      LOG_F(1,
            "Update statistics for %s/%s (shares=%u, work=%s)",
            !login.empty() ? login.c_str() : "<none>",
            !workerId.empty() ? workerId.c_str() : "<none>",
            acc.Current.SharesNum,
            acc.Current.SharesWork.getDecimal().c_str());

    // Update in-memory data
    acc.Current.Time.TimeBegin = acc.Time.TimeBegin;
    acc.Current.Time.TimeEnd = currentTime;
    acc.Recent.push_back(acc.Current);

    // Update on-disk data
    // Update [user,worker,time] -> state database
    if (acc.Current.SharesNum)
      writeStatsToDb(login, workerId, acc.Current);
    writeStatsToCache(login, workerId, acc.Current, acc.Time, statsFileData);
  }

  // Reset current worker state
  acc.Current.reset();
  acc.Time.TimeBegin = currentTime;

  // Remove old data
  Timestamp removeTimePoint = currentTime - std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticKeepTime);
  while (!acc.Recent.empty() && acc.Recent.front().Time.TimeEnd < removeTimePoint)
    acc.Recent.pop_front();
}

void StatisticDb::calcAverageMetrics(const StatisticDb::CStatsAccumulator &acc, std::chrono::seconds calculateInterval, std::chrono::seconds aggregateTime, CStats &result)
{
  // Calculate sum of shares number and work for last N minutes (interval usually defined in config)
  uint32_t primePOWTarget = acc.Current.PrimePOWTarget;
  uint32_t workerSharesNum = acc.Current.SharesNum;
  UInt<256> workerSharesWork = acc.Current.SharesWork;

  Timestamp startTimePoint = Timestamp::now();
  Timestamp stopTimePoint = startTimePoint - std::chrono::duration_cast<Timestamp::Duration>(calculateInterval);
  Timestamp lastTimePoint = startTimePoint - std::chrono::duration_cast<Timestamp::Duration>(aggregateTime);
  unsigned counter = 0;
  for (auto statsIt = acc.Recent.rbegin(), statsItEnd = acc.Recent.rend(); statsIt != statsItEnd; ++statsIt) {
    if (statsIt->Time.TimeEnd < stopTimePoint)
      break;

    lastTimePoint = statsIt->Time.TimeBegin;
    workerSharesNum += statsIt->SharesNum;
    workerSharesWork += statsIt->SharesWork;
    primePOWTarget = std::min(primePOWTarget, statsIt->PrimePOWTarget);
    counter++;
  }

  int64_t timeIntervalSeconds = (startTimePoint - lastTimePoint).count() / 1000;
  if (timeIntervalSeconds == 0)
    timeIntervalSeconds = 1;
  if (isDebugStatistic())
    LOG_F(1,
          "  * use %u statistic rounds; interval: %" PRIi64 "; shares num: %u; shares work: %s",
          counter,
          timeIntervalSeconds,
          workerSharesNum,
          workerSharesWork.getDecimal().c_str());

  result.SharesPerSecond = (double)workerSharesNum / timeIntervalSeconds;
  result.AveragePower = CoinInfo_.calculateAveragePower(workerSharesWork, timeIntervalSeconds, primePOWTarget);
  result.SharesWork = workerSharesWork;
  result.LastShareTime = acc.Time.TimeEnd;
}

void StatisticDb::writeStatsToDb(const std::string &loginId, const std::string &workerId, const CStatsElement &element)
{
  StatsRecord record;
  record.Login = loginId;
  record.WorkerId = workerId;
  record.Time = element.Time;
  record.ShareCount = element.SharesNum;
  record.ShareWork = element.SharesWork;
  record.PrimePOWTarget = element.PrimePOWTarget;
  record.PrimePOWShareCount.assign(element.PrimePOWSharesNum.begin(), element.PrimePOWSharesNum.end());
  if (!loginId.empty())
    WorkerStatsDb_.put(record);
  else
    PoolStatsDb_.put(record);
}

void StatisticDb::writeStatsToCache(const std::string &loginId, const std::string &workerId, const CStatsElement &element, const TimeInterval &time, xmstream &statsFileData)
{
  // TODO: use separate format for statistic cache record
  CStatsFileRecord record;
  record.Login = loginId;
  record.WorkerId = workerId;
  record.Time = time;
  record.ShareCount = element.SharesNum;
  record.ShareWork = element.SharesWork;
  record.PrimePOWTarget = element.PrimePOWTarget;
  record.PrimePOWShareCount.assign(element.PrimePOWSharesNum.begin(), element.PrimePOWSharesNum.end());
  DbIo<CStatsFileRecord>::serialize(statsFileData, record);
}

void StatisticDb::addShare(const CShare &share, bool updateWorkerAndUserStats, bool updatePoolStats)
{

  if (updateWorkerAndUserStats) {
    // Update worker stats
    LastWorkerStats_[makeWorkerStatsKey(share.userId, share.workerId)].addShare(share.WorkValue,
                                                                                share.Time,
                                                                                share.ChainLength,
                                                                                share.PrimePOWTarget,
                                                                                CoinInfo_.PowerUnitType == CCoinInfo::ECPD);
    // Update user stats
    LastWorkerStats_[makeWorkerStatsKey(share.userId, "")].addShare(share.WorkValue,
                                                                     share.Time,
                                                                     share.ChainLength,
                                                                     share.PrimePOWTarget,
                                                                     CoinInfo_.PowerUnitType == CCoinInfo::ECPD);
  }
  if (updatePoolStats) {
    // Update pool stats
    PoolStatsAcc_.addShare(share.WorkValue,
                           share.Time,
                           share.ChainLength,
                           share.PrimePOWTarget,
                           CoinInfo_.PowerUnitType == CCoinInfo::ECPD);
    PoolStatsCached_.LastShareTime = share.Time;
  }
  LastKnownShareId_ = std::max(LastKnownShareId_, share.UniqueShareId);
}

void StatisticDb::replayShare(const CShare &share)
{
  addShare(share,
           share.UniqueShareId > WorkersFlushInfo_.ShareId,
           share.UniqueShareId > PoolFlushInfo_.ShareId);

  if (isDebugStatistic()) {
    Dbg_.MinShareId = std::min(Dbg_.MinShareId, share.UniqueShareId);
    Dbg_.MaxShareId = std::max(Dbg_.MaxShareId, share.UniqueShareId);
    if (share.UniqueShareId > WorkersFlushInfo_.ShareId || share.UniqueShareId > PoolFlushInfo_.ShareId)
      Dbg_.Count++;
  }
}

void StatisticDb::initializationFinish(Timestamp timeLabel)
{
  if (isDebugStatistic()) {
    LOG_F(1, "initializationFinish: timeLabel: %" PRIi64 "", timeLabel.count());
    LOG_F(1, " * workers interval: %" PRIi64 " diff: %" PRIi64"",
          std::chrono::seconds(_cfg.StatisticWorkersAggregateTime).count(),
          (timeLabel - WorkersFlushInfo_.Time).count() / 1000);
    LOG_F(1, " * pool interval: %" PRIi64 " diff: %" PRIi64"",
          std::chrono::seconds(_cfg.StatisticPoolAggregateTime).count(),
          (timeLabel - PoolFlushInfo_.Time).count() / 1000);
  }

  if (timeLabel >= WorkersFlushInfo_.Time + std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticWorkersAggregateTime))
    updateWorkersStats(WorkersFlushInfo_.Time + std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticWorkersAggregateTime));
  if (timeLabel >= PoolFlushInfo_.Time + std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticPoolAggregateTime))
    updatePoolStats(PoolFlushInfo_.Time + std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticPoolAggregateTime));

  if (isDebugStatistic()) {
    LOG_F(1, "%s: replayed %" PRIu64 " shares from %" PRIu64 " to %" PRIu64 "", CoinInfo_.Name.c_str(), Dbg_.Count, Dbg_.MinShareId, Dbg_.MaxShareId);
  }
}

void StatisticDb::start()
{
  TaskHandler_.start();

  coroutineCall(coroutineNewWithCb([](void *arg) {
    StatisticDb *db = static_cast<StatisticDb*>(arg);
    for (;;) {
      ioSleep(db->WorkerStatsUpdaterEvent_, std::chrono::microseconds(db->_cfg.StatisticWorkersAggregateTime).count());
      if (db->ShutdownRequested_)
        break;
      db->updateWorkersStats(Timestamp::now());
    }
  }, this, 0x20000, coroutineFinishCb, &WorkerStatsUpdaterFinished_));

  coroutineCall(coroutineNewWithCb([](void *arg) {
    StatisticDb *db = static_cast<StatisticDb*>(arg);
    for (;;) {
      ioSleep(db->PoolStatsUpdaterEvent_, std::chrono::microseconds(db->_cfg.StatisticPoolAggregateTime).count());
      if (db->ShutdownRequested_)
        break;
      db->updatePoolStats(Timestamp::now());
    }
  }, this, 0x20000, coroutineFinishCb, &PoolStatsUpdaterFinished_));
}

void StatisticDb::stop()
{
  ShutdownRequested_ = true;
  userEventActivate(WorkerStatsUpdaterEvent_);
  userEventActivate(PoolStatsUpdaterEvent_);
  TaskHandler_.stop(CoinInfo_.Name.c_str(), "statisticDb task handler");
  coroutineJoin(CoinInfo_.Name.c_str(), "statisticDb worker stats updater", &WorkerStatsUpdaterFinished_);
  coroutineJoin(CoinInfo_.Name.c_str(), "statisticDb pool stats updater", &PoolStatsUpdaterFinished_);
}

void StatisticDb::updateWorkersStats(Timestamp timeLabel)
{
  xmstream statsFileData;
  for (auto it = LastWorkerStats_.begin(); it != LastWorkerStats_.end(); ) {
    auto [login, workerId] = splitWorkerStatsKey(it->first);
    CStatsAccumulator &acc = it->second;
    updateAcc(login, workerId, acc, timeLabel, statsFileData);
    if (acc.Recent.empty())
      it = LastWorkerStats_.erase(it);
    else
      ++it;
  }

  updateWorkersStatsDiskCache(timeLabel, LastKnownShareId_, statsFileData.data(), statsFileData.sizeOf());
}

void StatisticDb::updatePoolStats(Timestamp timeLabel)
{
  PoolStatsCached_.ClientsNum = 0;
  PoolStatsCached_.WorkersNum = 0;

  for (const auto &[key, acc]: LastWorkerStats_) {
    auto [login, workerId] = splitWorkerStatsKey(key);
    if (timeLabel - acc.Time.TimeEnd >= std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticWorkersPowerCalculateInterval))
      continue;
    if (workerId.empty())
      PoolStatsCached_.ClientsNum++;
    else
      PoolStatsCached_.WorkersNum++;
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

void StatisticDb::updateStatsDiskCache(const std::string &name, std::deque<CDatFile> &cache, Timestamp timeLabel, uint64_t lastShareId, const void *data, size_t size)
{
  // Don't write empty files to disk
  if (!size)
    return;

  CDatFile &statsFile = cache.emplace_back();
  statsFile.Path = _cfg.dbPath / name / (std::to_string(timeLabel.toUnixTime())+".dat");
  statsFile.LastShareId = lastShareId;
  statsFile.FileId = timeLabel.toUnixTime();

  FileDescriptor fd;
  if (!fd.open(statsFile.Path)) {
    LOG_F(ERROR, "StatisticDb: can't write file %s", path_to_utf8(statsFile.Path).c_str());
    return;
  }

  {
    uint32_t version = xhtole(static_cast<uint32_t>(CStatsFileData::CurrentRecordVersion));
    uint64_t serializedShareId = xhtole(lastShareId);
    fd.write(&version, sizeof(version));
    fd.write(&serializedShareId, sizeof(serializedShareId));
  }

  fd.write(data, size);
  fd.close();

  Timestamp removeTimePoint = timeLabel - std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticKeepTime);
  while (!cache.empty() && Timestamp::fromUnixTime(cache.front().FileId) < removeTimePoint) {
    if (isDebugStatistic())
      LOG_F(1, "Removing old statistic cache file %s", path_to_utf8(cache.front().Path).c_str());
    std::filesystem::remove(cache.front().Path);
    cache.pop_front();
  }
}

void StatisticDb::getUserStats(const std::string &user, CStats &userStats, std::vector<CStats> &workerStats, size_t offset, size_t size, EStatsColumn sortBy, bool sortDescending)
{
  auto lo = LastWorkerStats_.lower_bound(makeWorkerStatsKey(user, ""));
  if (lo == LastWorkerStats_.end() || splitWorkerStatsKey(lo->first).first != user)
    return;

  userStats.ClientsNum = 1;
  userStats.WorkersNum = 0;

  // Iterate over all workers
  std::vector<CStats> allStats;
  Timestamp lastShareTime;
  for (auto it = lo; it != LastWorkerStats_.end(); ++it) {
    auto [login, workerId] = splitWorkerStatsKey(it->first);
    if (login != user)
      break;
    const CStatsAccumulator &acc = it->second;
    CStats &result = allStats.emplace_back();
    result.WorkerId = std::move(workerId);
    if (isDebugStatistic())
      LOG_F(1, "Retrieve statistic for %s/%s", user.c_str(), result.WorkerId.c_str());
    calcAverageMetrics(acc, _cfg.StatisticWorkersPowerCalculateInterval, _cfg.StatisticWorkersAggregateTime, result);

    userStats.SharesPerSecond += result.SharesPerSecond;
    userStats.SharesWork += result.SharesWork;
    userStats.AveragePower += result.AveragePower;

    if (result.AveragePower)
      userStats.WorkersNum++;
    result.LastShareTime = acc.Time.TimeEnd;
    lastShareTime = std::max(lastShareTime, acc.Time.TimeEnd);
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
      current.SharesNum += static_cast<uint32_t>(valueRecord.ShareCount);
      current.SharesWork += valueRecord.ShareWork;
      current.PrimePOWTarget = std::min(current.PrimePOWTarget, valueRecord.PrimePOWTarget);
      if (current.PrimePOWSharesNum.size() < valueRecord.PrimePOWShareCount.size())
        current.PrimePOWSharesNum.resize(valueRecord.PrimePOWShareCount.size() + 1);
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
  result.clear();
  Timestamp timeLabel = Timestamp::now();
  for (const auto &[key, acc]: LastWorkerStats_) {
    auto [login, workerId] = splitWorkerStatsKey(key);
    if (!workerId.empty())
      continue;

    auto &userRecord = result.emplace_back();
    userRecord.UserId = std::move(login);
    // Current share work
    {
      auto &current = userRecord.Recent.emplace_back();
      current.SharesWork = acc.Current.SharesWork;
      current.Time.TimeEnd = timeLabel;
      current.Time.TimeBegin = acc.Time.TimeBegin;
    }

    // Recent share work (up to N minutes)
    Timestamp lastAcceptTime = timeLabel - std::chrono::minutes(30);
    for (auto It = acc.Recent.rbegin(), ItE = acc.Recent.rend(); It != ItE; ++It) {
      if (It->Time.TimeEnd < lastAcceptTime)
        break;
      auto &recent = userRecord.Recent.emplace_back();
      recent.SharesWork = It->SharesWork;
      recent.Time = It->Time;
    }
  }

  std::sort(result.begin(), result.end(), [](const CStatsExportData &l, const CStatsExportData &r) { return l.UserId < r.UserId; });
}

void StatisticDb::queryPoolStatsImpl(QueryPoolStatsCallback callback)
{
  callback(getPoolStats());
}

void StatisticDb::queryUserStatsImpl(const std::string &user, QueryUserStatsCallback callback, size_t offset, size_t size, StatisticDb::EStatsColumn sortBy, bool sortDescending)
{
  StatisticDb::CStats aggregate;
  std::vector<StatisticDb::CStats> workers;
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

    auto userIt = LastWorkerStats_.find(makeWorkerStatsKey(dst.Credentials.Login, ""));
    if (userIt == LastWorkerStats_.end())
      continue;

    CStats userStats;
    calcAverageMetrics(userIt->second, _cfg.StatisticWorkersPowerCalculateInterval, _cfg.StatisticWorkersAggregateTime, userStats);
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
    case CredentialsWithStatistic::ESharesPerSecord :
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
  Statistics_->addShare(*share, true, true);
}
