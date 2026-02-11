#include "poolcore/statsData.h"
#include "poolcommon/debug.h"
#include "poolcommon/path.h"
#include "loguru.hpp"
#include <algorithm>
#include <cinttypes>

template<typename T>
static void dbIoSerialize(xmstream &dst, const T &data) { DbIo<T>::serialize(dst, data); }
template<typename T>
static void dbIoUnserialize(xmstream &src, T &data) { DbIo<T>::unserialize(src, data); }
template<typename T>
static void dbKeyIoSerialize(xmstream &dst, const T &data) { DbKeyIo<T>::serialize(dst, data); }

void CStatsElement::reset()
{
  SharesNum = 0;
  SharesWork = UInt<256>::zero();
  Time = TimeInterval();
  PrimePOWTarget = -1U;
  PrimePOWSharesNum.clear();
}

void CStatsElement::merge(const CStatsElement &other)
{
  SharesNum += other.SharesNum;
  SharesWork += other.SharesWork;
  PrimePOWTarget = std::min(PrimePOWTarget, other.PrimePOWTarget);
  if (other.PrimePOWSharesNum.size() > PrimePOWSharesNum.size())
    PrimePOWSharesNum.resize(other.PrimePOWSharesNum.size());
  for (size_t i = 0; i < other.PrimePOWSharesNum.size(); i++)
    PrimePOWSharesNum[i] += other.PrimePOWSharesNum[i];
}

void CStatsSeries::addShare(const UInt<256> &workValue, Timestamp time, unsigned primeChainLength, unsigned primePOWTarget, bool isPrimePOW)
{
  Current.SharesNum++;
  Current.SharesWork += workValue;
  if (isPrimePOW) {
    Current.PrimePOWTarget = std::min(Current.PrimePOWTarget, primePOWTarget);
    primeChainLength = std::min(primeChainLength, 1024u);
    if (primeChainLength >= Current.PrimePOWSharesNum.size())
      Current.PrimePOWSharesNum.resize(primeChainLength + 1);
    Current.PrimePOWSharesNum[primeChainLength]++;
  }
  LastShareTime = time;
}

void CStatsSeries::merge(std::vector<CStatsElement> &cells)
{
  if (cells.empty())
    return;
  int64_t ci = static_cast<int64_t>(cells.size()) - 1;
  int64_t ri = static_cast<int64_t>(Recent.size()) - 1;
  while (ci >= 0) {
    if (ri >= 0 && cells[ci].Time.TimeEnd == Recent[ri].Time.TimeEnd) {
      Recent[ri--].merge(cells[ci--]);
    } else if (ri < 0 || cells[ci].Time.TimeEnd > Recent[ri].Time.TimeEnd) {
      // Insert at the correct sorted position; in practice only appends at the end
      Recent.insert(Recent.begin() + (ri + 1), std::move(cells[ci--]));
    } else {
      ri--;
    }
  }
  if (!Recent.empty())
    LastShareTime = std::max(LastShareTime, Recent.back().Time.TimeEnd);
}

void CStatsSeries::calcAverageMetrics(const CCoinInfo &coinInfo, std::chrono::seconds calculateInterval, std::chrono::seconds aggregateTime, CStats &result) const
{
  uint32_t primePOWTarget = Current.PrimePOWTarget;
  uint64_t sharesNum = Current.SharesNum;
  UInt<256> sharesWork = Current.SharesWork;

  Timestamp startTimePoint = Timestamp::now();
  Timestamp stopTimePoint = startTimePoint - std::chrono::duration_cast<Timestamp::Duration>(calculateInterval);
  Timestamp lastTimePoint = startTimePoint - std::chrono::duration_cast<Timestamp::Duration>(aggregateTime);
  unsigned counter = 0;
  for (auto statsIt = Recent.rbegin(), statsItEnd = Recent.rend(); statsIt != statsItEnd; ++statsIt) {
    if (statsIt->Time.TimeEnd < stopTimePoint)
      break;

    lastTimePoint = statsIt->Time.TimeBegin;
    sharesNum += statsIt->SharesNum;
    sharesWork += statsIt->SharesWork;
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
          sharesNum,
          sharesWork.getDecimal().c_str());

  result.SharesPerSecond = (double)sharesNum / timeIntervalSeconds;
  result.AveragePower = coinInfo.calculateAveragePower(sharesWork, timeIntervalSeconds, primePOWTarget);
  result.SharesWork = sharesWork;
  result.LastShareTime = LastShareTime;
}

std::vector<CStatsElement> CStatsSeries::flush(int64_t beginMs, int64_t endMs, int64_t gridIntervalMs, Timestamp removeTimePoint, std::set<int64_t> &removedTimes)
{
  std::vector<CStatsElement> cells;
  if (Current.SharesNum > 0 || !Current.SharesWork.isZero()) {
    cells = Current.distributeToGrid(beginMs, endMs, gridIntervalMs);
    merge(cells);
    Current.reset();
  }

  while (!Recent.empty() && Recent.front().Time.TimeEnd < removeTimePoint) {
    removedTimes.insert(Recent.front().Time.TimeEnd.count());
    Recent.pop_front();
  }

  return cells;
}

static void mergeStatsElement(const std::string &login, const std::string &workerId, const CStatsElement &element,
                              Timestamp timeLabel, kvdb<rocksdbBase>::Batch &batch)
{
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
}

void CStatsSeriesSingle::flush(int64_t beginMs, int64_t endMs, int64_t gridIntervalMs, Timestamp removeTimePoint,
                               Timestamp timeLabel, uint64_t lastShareId, kvdb<rocksdbBase>::Batch *batch,
                               std::set<int64_t> &modifiedTimes, std::set<int64_t> &removedTimes)
{
  LastShareId_ = lastShareId;
  if (isDebugStatistic() && (Series_.Current.SharesNum > 0 || !Series_.Current.SharesWork.isZero()))
    LOG_F(1, "Flush pool statistics (shares=%" PRIu64 ", work=%s)",
          Series_.Current.SharesNum,
          Series_.Current.SharesWork.getDecimal().c_str());

  auto cells = Series_.flush(beginMs, endMs, gridIntervalMs, removeTimePoint, removedTimes);
  if (batch) {
    for (const auto &element : cells) {
      mergeStatsElement("", "", element, timeLabel, *batch);
      modifiedTimes.insert(element.Time.TimeEnd.count());
    }
  }
}

void CStatsSeriesMap::flush(int64_t beginMs, int64_t endMs, int64_t gridIntervalMs, Timestamp removeTimePoint,
                            Timestamp timeLabel, uint64_t lastShareId, kvdb<rocksdbBase>::Batch *batch,
                            std::set<int64_t> &modifiedTimes, std::set<int64_t> &removedTimes)
{
  LastShareId_ = lastShareId;
  for (auto it = Map_.begin(); it != Map_.end(); ) {
    auto [login, workerId] = splitStatsKey(it->first);

    if (isDebugStatistic() && (it->second.Current.SharesNum > 0 || !it->second.Current.SharesWork.isZero()))
      LOG_F(1, "Flush statistics for %s/%s (shares=%" PRIu64 ", work=%s)",
            !login.empty() ? login.c_str() : "<none>",
            !workerId.empty() ? workerId.c_str() : "<none>",
            it->second.Current.SharesNum,
            it->second.Current.SharesWork.getDecimal().c_str());

    auto cells = it->second.flush(beginMs, endMs, gridIntervalMs, removeTimePoint, removedTimes);
    if (batch) {
      for (const auto &element : cells) {
        mergeStatsElement(login, workerId, element, timeLabel, *batch);
        modifiedTimes.insert(element.Time.TimeEnd.count());
      }
    }

    if (it->second.Recent.empty())
      it = Map_.erase(it);
    else
      ++it;
  }
}

static int64_t alignUpToGrid(int64_t timeMs, int64_t gridIntervalMs)
{
  int64_t gridSec = gridIntervalMs / 1000;
  int64_t timeSec = timeMs / 1000;
  return ((timeSec + gridSec - 1) / gridSec) * gridSec * 1000;
}

std::vector<CStatsElement> CStatsElement::distributeToGrid(int64_t beginMs, int64_t endMs, int64_t gridIntervalMs) const
{
  std::vector<CStatsElement> results;
  if (beginMs >= endMs || (SharesNum == 0 && SharesWork.isZero()))
    return results;

  int64_t totalMs = endMs - beginMs;
  int64_t firstGridEnd = alignUpToGrid(beginMs + 1, gridIntervalMs);
  int64_t lastGridEnd = alignUpToGrid(endMs, gridIntervalMs);

  uint64_t remainingShares = SharesNum;
  UInt<256> remainingWork = SharesWork;
  std::vector<uint32_t> remainingPrimePOW = PrimePOWSharesNum;

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
    element.PrimePOWTarget = PrimePOWTarget;

    if (gridEnd >= lastGridEnd) {
      // Last cell gets remainder
      element.SharesNum = remainingShares;
      element.SharesWork = remainingWork;
      element.PrimePOWSharesNum = remainingPrimePOW;
    } else {
      double fraction = static_cast<double>(overlapMs) / static_cast<double>(totalMs);
      element.SharesNum = static_cast<uint64_t>(std::round(SharesNum * fraction));
      remainingShares -= element.SharesNum;

      element.SharesWork = SharesWork;
      element.SharesWork.mulfp(fraction);
      remainingWork -= element.SharesWork;

      element.PrimePOWSharesNum.resize(PrimePOWSharesNum.size());
      for (size_t i = 0; i < PrimePOWSharesNum.size(); i++) {
        uint32_t allocated = static_cast<uint32_t>(std::round(PrimePOWSharesNum[i] * fraction));
        element.PrimePOWSharesNum[i] = allocated;
        remainingPrimePOW[i] -= allocated;
      }
    }

    results.push_back(element);
  }

  return results;
}

static bool parseStatsCacheFile(const std::string &coinName, CDatFile &file, CStatsFileData &fileData)
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
    LOG_F(ERROR, "<%s> StatisticDb: corrupted file %s", coinName.c_str(), path_to_utf8(file.Path).c_str());
    return false;
  }

  file.LastShareId = fileData.LastShareId;
  LOG_F(INFO, "<%s> Statistic cache file %s loaded successfully", coinName.c_str(), path_to_utf8(file.Path).c_str());
  return true;
}

void CStatsSeriesSingle::load(const std::filesystem::path &dbPath, const std::string &coinName)
{
  std::deque<CDatFile> files;
  enumerateDatFiles(files, dbPath / CachePath_, 3, true);

  for (auto &file : files) {
    CStatsFileData fileData;
    if (!parseStatsCacheFile(coinName, file, fileData)) {
      std::filesystem::remove(file.Path);
      continue;
    }

    if (fileData.Records.size() != 1) {
      LOG_F(ERROR, "<%s> Pool stats file has %zu records, expected 1: %s", coinName.c_str(), fileData.Records.size(), path_to_utf8(file.Path).c_str());
      std::filesystem::remove(file.Path);
      continue;
    }

    const auto &record = fileData.Records[0];
    if (!record.Login.empty() || !record.WorkerId.empty()) {
      LOG_F(ERROR, "<%s> Pool stats record has non-empty login/workerId: %s", coinName.c_str(), path_to_utf8(file.Path).c_str());
      std::filesystem::remove(file.Path);
      continue;
    }

    Series_.Recent.push_back(record.Element);
    Series_.LastShareTime = record.Element.Time.TimeEnd;

    if (isDebugStatistic()) {
      LOG_F(1, "<%s> Loaded pool stats: shares: %" PRIu64 " work: %s",
            coinName.c_str(),
            record.Element.SharesNum,
            record.Element.SharesWork.getDecimal().c_str());
    }

    LastShareId_ = std::max(LastShareId_, file.LastShareId);
  }
}

void CStatsSeriesMap::load(const std::filesystem::path &dbPath, const std::string &coinName)
{
  std::deque<CDatFile> files;
  enumerateDatFiles(files, dbPath / CachePath_, 3, true);

  for (auto &file : files) {
    CStatsFileData fileData;
    if (!parseStatsCacheFile(coinName, file, fileData)) {
      std::filesystem::remove(file.Path);
      continue;
    }

    auto hint = Map_.begin();
    for (const auto &record : fileData.Records) {
      auto key = makeStatsKey(record.Login, record.WorkerId);
      hint = Map_.try_emplace(hint, std::move(key));
      auto &acc = hint->second;
      acc.Recent.push_back(record.Element);
      acc.LastShareTime = record.Element.Time.TimeEnd;

      if (isDebugStatistic()) {
        LOG_F(1, "<%s> Loaded: %s/%s shares: %" PRIu64 " work: %s",
              coinName.c_str(),
              !record.Login.empty() ? record.Login.c_str() : "<empty>",
              !record.WorkerId.empty() ? record.WorkerId.c_str() : "<empty>",
              record.Element.SharesNum,
              record.Element.SharesWork.getDecimal().c_str());
      }
    }

    LastShareId_ = std::max(LastShareId_, file.LastShareId);
  }
}

static void writeStatsRecord(const std::string &login, const std::string &workerId, const CStatsElement &element, xmstream &out)
{
  CStatsFileRecord record;
  record.Login = login;
  record.WorkerId = workerId;
  record.Element = element;
  DbIo<CStatsFileRecord>::serialize(out, record);
}

static std::filesystem::path datFilePath(const std::filesystem::path &dbPath, const std::string &cachePath, int64_t gridEndMs)
{
  return dbPath / cachePath / (std::to_string(gridEndMs / 1000) + ".dat");
}

void CStatsSeriesSingle::rebuildDatFile(const std::filesystem::path &dbPath, int64_t gridEndMs)
{
  Timestamp gridEnd(gridEndMs);
  for (auto it = Series_.Recent.rbegin(); it != Series_.Recent.rend(); ++it) {
    if (it->Time.TimeEnd == gridEnd) {
      xmstream stream;
      DbIo<uint32_t>::serialize(stream, CStatsFileData::CurrentRecordVersion);
      DbIo<uint64_t>::serialize(stream, LastShareId_);
      DbIo<VarSize>::serialize(stream, VarSize(1));
      writeStatsRecord("", "", *it, stream);

      auto filePath = datFilePath(dbPath, CachePath_, gridEndMs);
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

void removeDatFile(const std::filesystem::path &dbPath, const std::string &cachePath, int64_t gridEndMs)
{
  auto filePath = datFilePath(dbPath, cachePath, gridEndMs);
  if (isDebugStatistic())
    LOG_F(1, "Removing old statistic cache file %s", path_to_utf8(filePath).c_str());
  std::filesystem::remove(filePath);
}

void CStatsSeriesMap::rebuildDatFile(const std::filesystem::path &dbPath, int64_t gridEndMs)
{
  xmstream recordsData;
  Timestamp gridEnd(gridEndMs);
  size_t recordCount = 0;
  for (const auto &[key, acc] : Map_) {
    for (auto it = acc.Recent.rbegin(); it != acc.Recent.rend(); ++it) {
      if (it->Time.TimeEnd == gridEnd) {
        auto [login, workerId] = splitStatsKey(key);
        writeStatsRecord(login, workerId, *it, recordsData);
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
  DbIo<uint64_t>::serialize(header, LastShareId_);
  DbIo<VarSize>::serialize(header, recordCount);

  auto filePath = datFilePath(dbPath, CachePath_, gridEndMs);
  FileDescriptor fd;
  if (!fd.open(filePath)) {
    LOG_F(ERROR, "StatisticDb: can't write file %s", path_to_utf8(filePath).c_str());
    return;
  }
  fd.write(header.data(), header.sizeOf());
  fd.write(recordsData.data(), recordsData.sizeOf());
  fd.close();
}

void CStatsSeriesMap::exportRecentStats(Timestamp accumulationBegin, std::vector<CStatsExportData> &result) const
{
  result.clear();
  Timestamp timeLabel = Timestamp::now();
  for (const auto &[key, acc]: Map_) {
    auto &userRecord = result.emplace_back();
    userRecord.UserId = splitStatsKey(key).first;
    // Current share work
    {
      auto &current = userRecord.Recent.emplace_back();
      current.SharesWork = acc.Current.SharesWork;
      current.Time.TimeEnd = timeLabel;
      current.Time.TimeBegin = accumulationBegin;
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

// ====================== StatsRecord ======================

bool StatsRecord::deserializeValue(xmstream &stream)
{
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version >= 1) {
    dbIoUnserialize(stream, Login);
    dbIoUnserialize(stream, WorkerId);
    dbIoUnserialize(stream, Time);
    dbIoUnserialize(stream, UpdateTime);
    dbIoUnserialize(stream, ShareCount);
    dbIoUnserialize(stream, ShareWork);
    dbIoUnserialize(stream, PrimePOWTarget);
    dbIoUnserialize(stream, PrimePOWShareCount);
  }

  return !stream.eof();
}

bool StatsRecord::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  return deserializeValue(stream);
}

void StatsRecord::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, Login);
  dbKeyIoSerialize(stream, WorkerId);
  DbKeyIo<Timestamp>::serialize(stream, Time.TimeEnd);
}

void StatsRecord::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, Login);
  dbIoSerialize(stream, WorkerId);
  dbIoSerialize(stream, Time);
  dbIoSerialize(stream, UpdateTime);
  dbIoSerialize(stream, ShareCount);
  dbIoSerialize(stream, ShareWork);
  dbIoSerialize(stream, PrimePOWTarget);
  dbIoSerialize(stream, PrimePOWShareCount);
}
