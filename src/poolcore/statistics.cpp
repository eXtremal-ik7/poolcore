#include "poolcore/statistics.h"
#include "poolcore/accounting.h"
#include "poolcore/statsMergeOperator.h"
#include "poolcommon/coroutineJoin.h"
#include "poolcommon/debug.h"
#include "poolcommon/path.h"
#include "loguru.hpp"
#include <algorithm>
#include <cmath>

bool StatisticDb::parseStatsCacheFile(CDatFile &file, CStatsFileData &fileData)
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
  DbIo<CStatsFileData>::unserialize(stream, fileData);

  if (stream.remaining() || stream.eof()) {
    LOG_F(ERROR, "<%s> StatisticDb: corrupted file %s", CoinInfo_.Name.c_str(), path_to_utf8(file.Path).c_str());
    return false;
  }

  file.LastShareId = fileData.LastShareId;
  LOG_F(INFO, "<%s> Statistic cache file %s loaded successfully", CoinInfo_.Name.c_str(), path_to_utf8(file.Path).c_str());
  return true;
}

CFlushInfo StatisticDb::loadPoolStatsFromDir(const std::filesystem::path &dirPath, CStatsAccumulator &acc)
{
  CFlushInfo result{};
  std::deque<CDatFile> files;
  enumerateDatFiles(files, dirPath, 3, true);

  for (auto &file : files) {
    CStatsFileData fileData;
    if (!parseStatsCacheFile(file, fileData)) {
      std::filesystem::remove(file.Path);
      continue;
    }

    if (fileData.Records.size() != 1) {
      LOG_F(ERROR, "<%s> Pool stats file has %zu records, expected 1: %s", CoinInfo_.Name.c_str(), fileData.Records.size(), path_to_utf8(file.Path).c_str());
      std::filesystem::remove(file.Path);
      continue;
    }

    const auto &record = fileData.Records[0];
    if (!record.Login.empty() || !record.WorkerId.empty()) {
      LOG_F(ERROR, "<%s> Pool stats record has non-empty login/workerId: %s", CoinInfo_.Name.c_str(), path_to_utf8(file.Path).c_str());
      std::filesystem::remove(file.Path);
      continue;
    }

    acc.Recent.push_back(record.Element);
    acc.LastShareTime = record.Element.Time.TimeEnd;

    if (isDebugStatistic()) {
      LOG_F(1, "<%s> Loaded pool stats: shares: %" PRIu64 " work: %s",
            CoinInfo_.Name.c_str(),
            record.Element.SharesNum,
            record.Element.SharesWork.getDecimal().c_str());
    }

    result.Time = Timestamp::fromUnixTime(file.FileId);
    result.ShareId = std::max(result.ShareId, file.LastShareId);
  }

  return result;
}

CFlushInfo StatisticDb::loadMapStatsFromDir(const std::filesystem::path &dirPath, std::map<std::string, CStatsAccumulator> &accMap)
{
  CFlushInfo result{};
  std::deque<CDatFile> files;
  enumerateDatFiles(files, dirPath, 3, true);

  for (auto &file : files) {
    CStatsFileData fileData;
    if (!parseStatsCacheFile(file, fileData)) {
      std::filesystem::remove(file.Path);
      continue;
    }

    auto hint = accMap.begin();
    for (const auto &record : fileData.Records) {
      auto key = makeWorkerStatsKey(record.Login, record.WorkerId);
      hint = accMap.try_emplace(hint, std::move(key));
      auto &acc = hint->second;
      acc.Recent.push_back(record.Element);
      acc.LastShareTime = record.Element.Time.TimeEnd;

      if (isDebugStatistic()) {
        LOG_F(1, "<%s> Loaded: %s/%s shares: %" PRIu64 " work: %s",
              CoinInfo_.Name.c_str(),
              !record.Login.empty() ? record.Login.c_str() : "<empty>",
              !record.WorkerId.empty() ? record.WorkerId.c_str() : "<empty>",
              record.Element.SharesNum,
              record.Element.SharesWork.getDecimal().c_str());
      }
    }

    result.Time = Timestamp::fromUnixTime(file.FileId);
    result.ShareId = std::max(result.ShareId, file.LastShareId);
  }

  return result;
}

StatisticDb::StatisticDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo) : _cfg(config), CoinInfo_(coinInfo),
  StatsDb_(_cfg.dbPath / "statistic", std::make_shared<StatsRecordMergeOperator>()),
  TaskHandler_(this, base)
{
  FlushEvent_ = newUserEvent(base, 1, nullptr, nullptr);

  Timestamp currentTime = Timestamp::now();
  FlushInfo_.Time = currentTime;
  FlushInfo_.ShareId = 0;

  auto applyResult = [this](CFlushInfo result) {
    if (result.Time.count() > 0) {
      FlushInfo_.Time = result.Time;
      FlushInfo_.ShareId = std::max(FlushInfo_.ShareId, result.ShareId);
    }
  };

  applyResult(loadMapStatsFromDir(_cfg.dbPath / CurrentWorkersStoragePath, LastWorkerStats_));
  applyResult(loadMapStatsFromDir(_cfg.dbPath / CurrentUsersStoragePath, LastUserStats_));
  applyResult(loadPoolStatsFromDir(_cfg.dbPath / CurrentPoolStoragePath, PoolStatsAcc_));

  LastKnownShareId_ = FlushInfo_.ShareId;
  if (isDebugStatistic())
    LOG_F(1, "%s: last aggregated id: %" PRIu64 " last known id: %" PRIu64 "", coinInfo.Name.c_str(), lastAggregatedShareId(), lastKnownShareId());
}

static int64_t alignUpToGrid(int64_t timeMs, int64_t gridIntervalMs)
{
  int64_t gridSec = gridIntervalMs / 1000;
  int64_t timeSec = timeMs / 1000;
  return ((timeSec + gridSec - 1) / gridSec) * gridSec * 1000;
}

std::vector<StatisticDb::CStatsElement> StatisticDb::distributeToGrid(const CStatsElement &current, int64_t beginMs, int64_t endMs, int64_t gridIntervalMs)
{
  std::vector<CStatsElement> results;
  if (beginMs >= endMs || (current.SharesNum == 0 && current.SharesWork.isZero()))
    return results;

  int64_t totalMs = endMs - beginMs;
  int64_t firstGridEnd = alignUpToGrid(beginMs + 1, gridIntervalMs);
  int64_t lastGridEnd = alignUpToGrid(endMs, gridIntervalMs);

  uint64_t remainingShares = current.SharesNum;
  UInt<256> remainingWork = current.SharesWork;
  std::vector<uint32_t> remainingPrimePOW = current.PrimePOWSharesNum;

  for (int64_t gridEnd = firstGridEnd; gridEnd <= lastGridEnd; gridEnd += gridIntervalMs) {
    int64_t gridStart = gridEnd - gridIntervalMs;
    int64_t overlapBegin = std::max(beginMs, gridStart);
    int64_t overlapEnd = std::min(endMs, gridEnd);
    int64_t overlapMs = overlapEnd - overlapBegin;
    if (overlapMs <= 0)
      continue;

    CStatsElement element;
    element.Time.TimeBegin = Timestamp(gridStart);
    element.Time.TimeEnd = Timestamp(gridEnd);
    element.PrimePOWTarget = current.PrimePOWTarget;

    if (gridEnd >= lastGridEnd) {
      // Last cell gets remainder
      element.SharesNum = remainingShares;
      element.SharesWork = remainingWork;
      element.PrimePOWSharesNum = remainingPrimePOW;
    } else {
      double fraction = static_cast<double>(overlapMs) / static_cast<double>(totalMs);
      element.SharesNum = static_cast<uint64_t>(std::round(current.SharesNum * fraction));
      remainingShares -= element.SharesNum;

      element.SharesWork = current.SharesWork;
      element.SharesWork.mulfp(fraction);
      remainingWork -= element.SharesWork;

      element.PrimePOWSharesNum.resize(current.PrimePOWSharesNum.size());
      for (size_t i = 0; i < current.PrimePOWSharesNum.size(); i++) {
        uint32_t allocated = static_cast<uint32_t>(std::round(current.PrimePOWSharesNum[i] * fraction));
        element.PrimePOWSharesNum[i] = allocated;
        remainingPrimePOW[i] -= allocated;
      }
    }

    results.push_back(element);
  }

  return results;
}

void StatisticDb::flushAccumulator(const std::string &login, const std::string &workerId, CStatsAccumulator &acc, int64_t gridIntervalMs, Timestamp timeLabel, kvdb<rocksdbBase>::Batch &batch, std::set<int64_t> &modifiedTimes, std::set<int64_t> &removedTimes)
{
  if (acc.Current.SharesNum > 0 || !acc.Current.SharesWork.isZero()) {
    if (isDebugStatistic())
      LOG_F(1, "Flush statistics for %s/%s (shares=%" PRIu64 ", work=%s)",
            !login.empty() ? login.c_str() : "<none>",
            !workerId.empty() ? workerId.c_str() : "<none>",
            acc.Current.SharesNum,
            acc.Current.SharesWork.getDecimal().c_str());

    auto cells = distributeToGrid(acc.Current, AccumulationBegin_.count(), timeLabel.count(), gridIntervalMs);

    for (const auto &element : cells) {
      StatsRecord record;
      record.Login = login;
      record.WorkerId = workerId;
      record.Time = element.Time;
      record.UpdateTime = timeLabel;
      record.ShareCount = element.SharesNum;
      record.ShareWork = element.SharesWork;
      record.PrimePOWTarget = element.PrimePOWTarget;
      record.PrimePOWShareCount.assign(element.PrimePOWSharesNum.begin(), element.PrimePOWSharesNum.end());
      batch.merge(record);
      modifiedTimes.insert(element.Time.TimeEnd.count());
    }

    acc.merge(cells);
    acc.Current.reset();
  }

  // Remove old data
  Timestamp removeTimePoint = timeLabel - std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticKeepTime);
  while (!acc.Recent.empty() && acc.Recent.front().Time.TimeEnd < removeTimePoint) {
    removedTimes.insert(acc.Recent.front().Time.TimeEnd.count());
    acc.Recent.pop_front();
  }
}

void StatisticDb::updatePoolStatsCached(Timestamp timeLabel)
{
  PoolStatsCached_.ClientsNum = 0;
  PoolStatsCached_.WorkersNum = 0;

  for (const auto &[key, acc]: LastUserStats_) {
    if (timeLabel - acc.LastShareTime < std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticWorkersPowerCalculateInterval))
      PoolStatsCached_.ClientsNum++;
  }

  for (const auto &[key, acc]: LastWorkerStats_) {
    if (timeLabel - acc.LastShareTime < std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticWorkersPowerCalculateInterval))
      PoolStatsCached_.WorkersNum++;
  }

  if (isDebugStatistic())
    LOG_F(1, "update pool stats:");
  calcAverageMetrics(PoolStatsAcc_, _cfg.StatisticPoolPowerCalculateInterval, _cfg.StatisticPoolGridInterval, PoolStatsCached_);

  LOG_F(INFO,
        "clients: %u, workers: %u, power: %" PRIu64 ", share rate: %.3lf shares/s",
        PoolStatsCached_.ClientsNum,
        PoolStatsCached_.WorkersNum,
        PoolStatsCached_.AveragePower,
        PoolStatsCached_.SharesPerSecond);
}

void StatisticDb::rebuildDatFile(const std::string &cachePath, int64_t gridEndMs, const std::map<std::string, CStatsAccumulator> &accMap)
{
  xmstream recordsData;
  Timestamp gridEnd(gridEndMs);
  size_t recordCount = 0;
  for (const auto &[key, acc] : accMap) {
    for (auto it = acc.Recent.rbegin(); it != acc.Recent.rend(); ++it) {
      if (it->Time.TimeEnd == gridEnd) {
        auto [login, workerId] = splitWorkerStatsKey(key);
        writeStatsToCache(login, workerId, *it, recordsData);
        recordCount++;
        break;
      }
      if (it->Time.TimeEnd < gridEnd)
        break;
    }
  }

  if (recordCount == 0)
    return;

  xmstream header;
  DbIo<uint32_t>::serialize(header, CStatsFileData::CurrentRecordVersion);
  DbIo<uint64_t>::serialize(header, LastKnownShareId_);
  DbIo<VarSize>::serialize(header, recordCount);

  std::filesystem::path filePath = _cfg.dbPath / cachePath / (std::to_string(gridEndMs / 1000) + ".dat");
  FileDescriptor fd;
  if (!fd.open(filePath)) {
    LOG_F(ERROR, "StatisticDb: can't write file %s", path_to_utf8(filePath).c_str());
    return;
  }
  fd.write(header.data(), header.sizeOf());
  fd.write(recordsData.data(), recordsData.sizeOf());
  fd.close();
}

void StatisticDb::rebuildDatFile(const std::string &cachePath, int64_t gridEndMs, const std::string &login, const std::string &workerId, const CStatsAccumulator &acc)
{
  Timestamp gridEnd(gridEndMs);
  for (auto it = acc.Recent.rbegin(); it != acc.Recent.rend(); ++it) {
    if (it->Time.TimeEnd == gridEnd) {
      const auto &el = *it;
      xmstream stream;
      DbIo<uint32_t>::serialize(stream, CStatsFileData::CurrentRecordVersion);
      DbIo<uint64_t>::serialize(stream, LastKnownShareId_);
      DbIo<VarSize>::serialize(stream, VarSize(1));
      writeStatsToCache(login, workerId, el, stream);

      std::filesystem::path filePath = _cfg.dbPath / cachePath / (std::to_string(gridEndMs / 1000) + ".dat");
      FileDescriptor fd;
      if (!fd.open(filePath)) {
        LOG_F(ERROR, "StatisticDb: can't write file %s", path_to_utf8(filePath).c_str());
        return;
      }
      fd.write(stream.data(), stream.sizeOf());
      fd.close();
      return;
    }
    if (it->Time.TimeEnd < gridEnd)
      break;
  }
}

void StatisticDb::flushAll(Timestamp timeLabel)
{
  int64_t workerGridMs = std::chrono::duration_cast<std::chrono::milliseconds>(_cfg.StatisticWorkerGridInterval).count();
  int64_t userGridMs = std::chrono::duration_cast<std::chrono::milliseconds>(_cfg.StatisticUserGridInterval).count();
  int64_t poolGridMs = std::chrono::duration_cast<std::chrono::milliseconds>(_cfg.StatisticPoolGridInterval).count();

  kvdb<rocksdbBase>::Batch batch;
  std::set<int64_t> modifiedWorkerTimes;
  std::set<int64_t> modifiedUserTimes;
  std::set<int64_t> modifiedPoolTimes;
  std::set<int64_t> removedWorkerTimes;
  std::set<int64_t> removedUserTimes;
  std::set<int64_t> removedPoolTimes;

  // Flush workers
  for (auto it = LastWorkerStats_.begin(); it != LastWorkerStats_.end(); ) {
    auto [login, workerId] = splitWorkerStatsKey(it->first);
    flushAccumulator(login, workerId, it->second, workerGridMs, timeLabel, batch, modifiedWorkerTimes, removedWorkerTimes);
    if (it->second.Recent.empty())
      it = LastWorkerStats_.erase(it);
    else
      ++it;
  }

  // Flush users
  for (auto it = LastUserStats_.begin(); it != LastUserStats_.end(); ) {
    auto [login, workerId] = splitWorkerStatsKey(it->first);
    flushAccumulator(login, workerId, it->second, userGridMs, timeLabel, batch, modifiedUserTimes, removedUserTimes);
    if (it->second.Recent.empty())
      it = LastUserStats_.erase(it);
    else
      ++it;
  }

  // Flush pool
  flushAccumulator("", "", PoolStatsAcc_, poolGridMs, timeLabel, batch, modifiedPoolTimes, removedPoolTimes);

  // Write batch to RocksDB
  StatsDb_.writeBatch(batch);

  // Rebuild .dat files for modified grid times
  for (int64_t gridEndMs : modifiedWorkerTimes)
    rebuildDatFile(CurrentWorkersStoragePath, gridEndMs, LastWorkerStats_);
  for (int64_t gridEndMs : modifiedUserTimes)
    rebuildDatFile(CurrentUsersStoragePath, gridEndMs, LastUserStats_);
  for (int64_t gridEndMs : modifiedPoolTimes)
    rebuildDatFile(CurrentPoolStoragePath, gridEndMs, "", "", PoolStatsAcc_);

  // Remove old .dat files
  auto removeDatFiles = [this](const std::string &cachePath, const std::set<int64_t> &times) {
    for (int64_t gridEndMs : times) {
      auto filePath = _cfg.dbPath / cachePath / (std::to_string(gridEndMs / 1000) + ".dat");
      if (isDebugStatistic())
        LOG_F(1, "Removing old statistic cache file %s", path_to_utf8(filePath).c_str());
      std::filesystem::remove(filePath);
    }
  };
  removeDatFiles(CurrentWorkersStoragePath, removedWorkerTimes);
  removeDatFiles(CurrentUsersStoragePath, removedUserTimes);
  removeDatFiles(CurrentPoolStoragePath, removedPoolTimes);

  // Update cached pool stats
  updatePoolStatsCached(timeLabel);

  AccumulationBegin_ = timeLabel;
  FlushInfo_.Time = timeLabel;
  FlushInfo_.ShareId = LastKnownShareId_;
}

void StatisticDb::calcAverageMetrics(const StatisticDb::CStatsAccumulator &acc, std::chrono::seconds calculateInterval, std::chrono::seconds aggregateTime, CStats &result)
{
  // Calculate sum of shares number and work for last N minutes (interval usually defined in config)
  uint32_t primePOWTarget = acc.Current.PrimePOWTarget;
  uint64_t workerSharesNum = acc.Current.SharesNum;
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
          "  * use %u statistic rounds; interval: %" PRIi64 "; shares num: %" PRIu64 "; shares work: %s",
          counter,
          timeIntervalSeconds,
          workerSharesNum,
          workerSharesWork.getDecimal().c_str());

  result.SharesPerSecond = (double)workerSharesNum / timeIntervalSeconds;
  result.AveragePower = CoinInfo_.calculateAveragePower(workerSharesWork, timeIntervalSeconds, primePOWTarget);
  result.SharesWork = workerSharesWork;
  result.LastShareTime = acc.LastShareTime;
}

void StatisticDb::writeStatsToCache(const std::string &loginId, const std::string &workerId, const CStatsElement &element, xmstream &statsFileData)
{
  CStatsFileRecord record;
  record.Login = loginId;
  record.WorkerId = workerId;
  record.Element = element;
  DbIo<CStatsFileRecord>::serialize(statsFileData, record);
}

void StatisticDb::addShare(const CShare &share)
{
  bool isPrimePOW = CoinInfo_.PowerUnitType == CCoinInfo::ECPD;
  auto workerKey = makeWorkerStatsKey(share.userId, share.workerId);
  auto userKey = makeWorkerStatsKey(share.userId, "");
  LastWorkerStats_[workerKey].addShare(share.WorkValue, share.Time, share.ChainLength, share.PrimePOWTarget, isPrimePOW);
  LastUserStats_[userKey].addShare(share.WorkValue, share.Time, share.ChainLength, share.PrimePOWTarget, isPrimePOW);
  PoolStatsAcc_.addShare(share.WorkValue, share.Time, share.ChainLength, share.PrimePOWTarget, isPrimePOW);
  PoolStatsCached_.LastShareTime = share.Time;
  LastKnownShareId_ = std::max(LastKnownShareId_, share.UniqueShareId);
}

void StatisticDb::replayShare(const CShare &share)
{
  if (share.UniqueShareId > FlushInfo_.ShareId)
    addShare(share);
  LastKnownShareId_ = std::max(LastKnownShareId_, share.UniqueShareId);

  if (isDebugStatistic()) {
    Dbg_.MinShareId = std::min(Dbg_.MinShareId, share.UniqueShareId);
    Dbg_.MaxShareId = std::max(Dbg_.MaxShareId, share.UniqueShareId);
    if (share.UniqueShareId > FlushInfo_.ShareId)
      Dbg_.Count++;
  }
}

void StatisticDb::initializationFinish(Timestamp timeLabel)
{
  AccumulationBegin_ = FlushInfo_.Time;

  if (isDebugStatistic()) {
    LOG_F(1, "initializationFinish: timeLabel: %" PRIi64 "", timeLabel.count());
    LOG_F(1, " * flush interval: %" PRIi64 " diff: %" PRIi64"",
          std::chrono::seconds(_cfg.StatisticFlushInterval).count(),
          (timeLabel - FlushInfo_.Time).count() / 1000);
  }

  if (timeLabel >= FlushInfo_.Time + std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticFlushInterval))
    flushAll(FlushInfo_.Time + std::chrono::duration_cast<Timestamp::Duration>(_cfg.StatisticFlushInterval));

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
  auto userIt = LastUserStats_.find(makeWorkerStatsKey(user, ""));
  auto workerLo = LastWorkerStats_.lower_bound(makeWorkerStatsKey(user, ""));
  bool hasWorkers = (workerLo != LastWorkerStats_.end() && splitWorkerStatsKey(workerLo->first).first == user);
  if (userIt == LastUserStats_.end() && !hasWorkers)
    return;

  userStats.ClientsNum = 1;
  userStats.WorkersNum = 0;

  std::vector<CStats> allStats;
  Timestamp lastShareTime;

  // User-level stats
  if (userIt != LastUserStats_.end()) {
    const CStatsAccumulator &acc = userIt->second;
    CStats &result = allStats.emplace_back();
    if (isDebugStatistic())
      LOG_F(1, "Retrieve statistic for %s/<user>", user.c_str());
    calcAverageMetrics(acc, _cfg.StatisticWorkersPowerCalculateInterval, _cfg.StatisticUserGridInterval, result);
    userStats.SharesPerSecond += result.SharesPerSecond;
    userStats.SharesWork += result.SharesWork;
    userStats.AveragePower += result.AveragePower;
    if (result.AveragePower)
      userStats.WorkersNum++;
    result.LastShareTime = acc.LastShareTime;
    lastShareTime = std::max(lastShareTime, acc.LastShareTime);
  }

  // Worker-level stats
  for (auto it = workerLo; it != LastWorkerStats_.end(); ++it) {
    auto [login, workerId] = splitWorkerStatsKey(it->first);
    if (login != user)
      break;
    const CStatsAccumulator &acc = it->second;
    CStats &result = allStats.emplace_back();
    result.WorkerId = std::move(workerId);
    if (isDebugStatistic())
      LOG_F(1, "Retrieve statistic for %s/%s", user.c_str(), result.WorkerId.c_str());
    calcAverageMetrics(acc, _cfg.StatisticWorkersPowerCalculateInterval, _cfg.StatisticWorkerGridInterval, result);
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
  result.clear();
  Timestamp timeLabel = Timestamp::now();
  for (const auto &[key, acc]: LastUserStats_) {
    auto &userRecord = result.emplace_back();
    userRecord.UserId = splitWorkerStatsKey(key).first;
    // Current share work
    {
      auto &current = userRecord.Recent.emplace_back();
      current.SharesWork = acc.Current.SharesWork;
      current.Time.TimeEnd = timeLabel;
      current.Time.TimeBegin = AccumulationBegin_;
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

    auto userIt = LastUserStats_.find(makeWorkerStatsKey(dst.Credentials.Login, ""));
    if (userIt == LastUserStats_.end())
      continue;

    CStats userStats;
    calcAverageMetrics(userIt->second, _cfg.StatisticWorkersPowerCalculateInterval, _cfg.StatisticWorkerGridInterval, userStats);
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
