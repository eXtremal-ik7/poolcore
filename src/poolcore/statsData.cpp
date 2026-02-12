#include "poolcore/statsData.h"
#include "poolcommon/debug.h"
#include "poolcommon/path.h"
#include "loguru.hpp"
#include <algorithm>
#include <cassert>
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

// Note: Time is not merged — callers always merge elements with matching time intervals
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

void CStats::merge(const StatsRecord &record)
{
  SharesNum += record.ShareCount;
  SharesWork += record.ShareWork;
  PrimePOWTarget = std::min(PrimePOWTarget, record.PrimePOWTarget);
}

void CStats::mergeScaled(const StatsRecord &record, double fraction)
{
  SharesNum += static_cast<uint64_t>(std::round(record.ShareCount * fraction));
  UInt<256> scaledWork = record.ShareWork;
  scaledWork.mulfp(fraction);
  SharesWork += scaledWork;
  PrimePOWTarget = std::min(PrimePOWTarget, record.PrimePOWTarget);
}

CStatsElement CStatsElement::scaled(double fraction) const
{
  CStatsElement result;
  result.SharesNum = static_cast<uint64_t>(std::round(SharesNum * fraction));
  result.SharesWork = SharesWork;
  result.SharesWork.mulfp(fraction);
  result.PrimePOWTarget = PrimePOWTarget;
  result.PrimePOWSharesNum.resize(PrimePOWSharesNum.size());
  for (size_t i = 0; i < PrimePOWSharesNum.size(); i++)
    result.PrimePOWSharesNum[i] = static_cast<uint64_t>(std::round(PrimePOWSharesNum[i] * fraction));
  return result;
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

void CStatsSeries::calcAverageMetrics(const CCoinInfo &coinInfo, std::chrono::seconds calculateInterval, Timestamp now, CStats &result) const
{
  Timestamp windowBegin = now - calculateInterval;

  uint32_t primePOWTarget = Current.PrimePOWTarget;
  uint64_t sharesNum = Current.SharesNum;
  UInt<256> sharesWork = Current.SharesWork;

  for (auto it = Recent.rbegin(), itEnd = Recent.rend(); it != itEnd; ++it) {
    if (it->Time.TimeEnd <= windowBegin)
      break;

    primePOWTarget = std::min(primePOWTarget, it->PrimePOWTarget);

    if (it->Time.TimeBegin >= windowBegin) {
      sharesNum += it->SharesNum;
      sharesWork += it->SharesWork;
    } else {
      // Straddles boundary — clip proportionally
      int64_t totalMs = it->Time.TimeEnd.count() - it->Time.TimeBegin.count();
      int64_t overlapMs = it->Time.TimeEnd.count() - windowBegin.count();
      double fraction = static_cast<double>(overlapMs) / static_cast<double>(totalMs);
      sharesNum += static_cast<uint64_t>(std::round(it->SharesNum * fraction));
      UInt<256> partialWork = it->SharesWork;
      partialWork.mulfp(fraction);
      sharesWork += partialWork;
      break;
    }
  }

  int64_t intervalSeconds = std::chrono::duration_cast<std::chrono::seconds>(calculateInterval).count();
  if (isDebugStatistic())
    LOG_F(1,
          "  * interval: %" PRIi64 "; shares num: %" PRIu64 "; shares work: %s",
          intervalSeconds,
          sharesNum,
          sharesWork.getDecimal().c_str());

  result.SharesPerSecond = (double)sharesNum / intervalSeconds;
  result.AveragePower = coinInfo.calculateAveragePower(sharesWork, intervalSeconds, primePOWTarget);
  result.SharesWork = sharesWork;
  result.LastShareTime = LastShareTime;
}

void CStatsSeries::flush(int64_t beginMs,
                         int64_t endMs,
                         int64_t gridIntervalMs,
                         Timestamp removeTimePoint,
                         std::string_view login,
                         std::string_view workerId,
                         Timestamp timeLabel,
                         kvdb<rocksdbBase>::Batch *batch,
                         std::set<int64_t> &modifiedTimes,
                         std::set<int64_t> &removedTimes)
{
  if (Current.SharesNum > 0 || !Current.SharesWork.isZero()) {
    auto cells = Current.distributeToGrid(beginMs, endMs, gridIntervalMs);
    merge(cells);
    Current.reset();
    for (const auto &element : cells) {
      if (batch) {
        StatsRecord record;
        record.Login = login;
        record.WorkerId = workerId;
        record.Time = element.Time;
        record.UpdateTime = timeLabel;
        record.ShareCount = element.SharesNum;
        record.ShareWork = element.SharesWork;
        record.PrimePOWTarget = element.PrimePOWTarget;
        record.PrimePOWShareCount = element.PrimePOWSharesNum;
        batch->merge(record);
      }
      modifiedTimes.insert(element.Time.TimeEnd.count());
    }
  }

  while (!Recent.empty() && Recent.front().Time.TimeEnd < removeTimePoint) {
    removedTimes.insert(Recent.front().Time.TimeEnd.count());
    Recent.pop_front();
  }
}

static int64_t alignUpToGrid(int64_t timeMs, int64_t gridIntervalMs)
{
  return ((timeMs + gridIntervalMs - 1) / gridIntervalMs) * gridIntervalMs;
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
  std::vector<uint64_t> remainingPrimePOW = PrimePOWSharesNum;

  for (int64_t gridEnd = firstGridEnd; gridEnd <= lastGridEnd; gridEnd += gridIntervalMs) {
    int64_t gridStart = gridEnd - gridIntervalMs;
    int64_t overlapBegin = std::max(beginMs, gridStart);
    int64_t overlapEnd = std::min(endMs, gridEnd);
    int64_t overlapMs = overlapEnd - overlapBegin;
    if (overlapMs <= 0)
      continue;

    CStatsElement element;
    if (gridEnd >= lastGridEnd) {
      // Last cell gets remainder
      element.SharesNum = remainingShares;
      element.SharesWork = remainingWork;
      element.PrimePOWTarget = PrimePOWTarget;
      element.PrimePOWSharesNum = remainingPrimePOW;
    } else {
      double fraction = static_cast<double>(overlapMs) / static_cast<double>(totalMs);
      element = scaled(fraction);
      element.SharesNum = std::min(element.SharesNum, remainingShares);
      remainingShares -= element.SharesNum;
      if (element.SharesWork > remainingWork)
        element.SharesWork = remainingWork;
      remainingWork -= element.SharesWork;
      for (size_t i = 0; i < PrimePOWSharesNum.size(); i++) {
        element.PrimePOWSharesNum[i] = std::min(element.PrimePOWSharesNum[i], remainingPrimePOW[i]);
        remainingPrimePOW[i] -= element.PrimePOWSharesNum[i];
      }
    }

    element.Time.TimeBegin = Timestamp(gridStart);
    element.Time.TimeEnd = Timestamp(gridEnd);
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

    SavedShareId_ = std::max(SavedShareId_, file.LastShareId);
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

    SavedShareId_ = std::max(SavedShareId_, file.LastShareId);
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

// gridEndMs is always a multiple of 1000 (grid intervals are whole seconds)
static std::filesystem::path datFilePath(const std::filesystem::path &dbPath, const std::string &cachePath, int64_t gridEndMs)
{
  assert(gridEndMs % 1000 == 0);
  return dbPath / cachePath / (std::to_string(gridEndMs / 1000) + ".dat");
}

void CStatsSeriesSingle::rebuildDatFile(const std::filesystem::path &dbPath, int64_t gridEndMs)
{
  Timestamp gridEnd(gridEndMs);
  for (auto it = Series_.Recent.rbegin(); it != Series_.Recent.rend(); ++it) {
    if (it->Time.TimeEnd == gridEnd) {
      xmstream stream;
      DbIo<CStatsFileData>::serializeHeader(stream, SavedShareId_, 1);
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

static void removeDatFile(const std::filesystem::path &dbPath, const std::string &cachePath, int64_t gridEndMs)
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
  DbIo<CStatsFileData>::serializeHeader(header, SavedShareId_, recordCount);

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

void CStatsSeriesSingle::flush(Timestamp timeLabel, uint64_t lastShareId,
                               const std::filesystem::path &dbPath, kvdb<rocksdbBase> *db)
{
  SavedShareId_ = lastShareId;
  if (isDebugStatistic() && (Series_.Current.SharesNum > 0 || !Series_.Current.SharesWork.isZero()))
    LOG_F(1, "Flush pool statistics (shares=%" PRIu64 ", work=%s)",
          Series_.Current.SharesNum,
          Series_.Current.SharesWork.getDecimal().c_str());

  int64_t gridIntervalMs = std::chrono::duration_cast<std::chrono::milliseconds>(GridInterval_).count();
  Timestamp removeTimePoint = timeLabel - KeepTime_;

  kvdb<rocksdbBase>::Batch batch;
  std::set<int64_t> modifiedTimes, removedTimes;
  Series_.flush(AccumulationBegin_.count(), timeLabel.count(), gridIntervalMs, removeTimePoint,
                "", "", timeLabel, db ? &batch : nullptr, modifiedTimes, removedTimes);

  if (db)
    db->writeBatch(batch);

  for (int64_t t : removedTimes)
    modifiedTimes.erase(t);
  for (int64_t t : modifiedTimes)
    rebuildDatFile(dbPath, t);
  for (int64_t t : removedTimes)
    removeDatFile(dbPath, CachePath_, t);

  AccumulationBegin_ = timeLabel;
}

void CStatsSeriesMap::flush(Timestamp timeLabel, uint64_t lastShareId,
                            const std::filesystem::path &dbPath, kvdb<rocksdbBase> *db)
{
  SavedShareId_ = lastShareId;
  int64_t gridIntervalMs = std::chrono::duration_cast<std::chrono::milliseconds>(GridInterval_).count();
  Timestamp removeTimePoint = timeLabel - KeepTime_;

  kvdb<rocksdbBase>::Batch batch;
  std::set<int64_t> modifiedTimes, removedTimes;
  for (auto it = Map_.begin(); it != Map_.end(); ) {
    auto [login, workerId] = splitStatsKey(it->first);

    if (isDebugStatistic() && (it->second.Current.SharesNum > 0 || !it->second.Current.SharesWork.isZero()))
      LOG_F(1, "Flush statistics for %s/%s (shares=%" PRIu64 ", work=%s)",
            !login.empty() ? login.c_str() : "<none>",
            !workerId.empty() ? workerId.c_str() : "<none>",
            it->second.Current.SharesNum,
            it->second.Current.SharesWork.getDecimal().c_str());

    it->second.flush(AccumulationBegin_.count(), timeLabel.count(), gridIntervalMs, removeTimePoint,
                     login, workerId, timeLabel, db ? &batch : nullptr, modifiedTimes, removedTimes);

    if (it->second.Recent.empty())
      it = Map_.erase(it);
    else
      ++it;
  }

  if (db)
    db->writeBatch(batch);

  // Don't rebuild .dat files for times that will be removed anyway (data aged past KeepTime)
  for (int64_t t : removedTimes)
    modifiedTimes.erase(t);
  for (int64_t t : modifiedTimes)
    rebuildDatFile(dbPath, t);
  for (int64_t t : removedTimes)
    removeDatFile(dbPath, CachePath_, t);

  AccumulationBegin_ = timeLabel;
}

void CStatsSeriesMap::exportRecentStats(std::chrono::seconds window, std::vector<CStatsExportData> &result) const
{
  result.clear();
  Timestamp timeLabel = Timestamp::now();
  for (const auto &[key, acc]: Map_) {
    // Skip users with no shares since last flush and no recent history in window
    Timestamp lastAcceptTime = timeLabel - window;
    bool hasRecent = !acc.Recent.empty() && acc.Recent.back().Time.TimeEnd >= lastAcceptTime;
    if (acc.Current.SharesNum == 0 && acc.Current.SharesWork.isZero() && !hasRecent)
      continue;

    auto &userRecord = result.emplace_back();
    userRecord.UserId = splitStatsKey(key).first;
    // Current share work
    if (acc.Current.SharesNum > 0 || !acc.Current.SharesWork.isZero()) {
      auto &current = userRecord.Recent.emplace_back();
      current.SharesWork = acc.Current.SharesWork;
      current.Time.TimeEnd = timeLabel;
      current.Time.TimeBegin = AccumulationBegin_;
    }

    // Recent share work (up to N minutes)
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
  if (version == 1) {
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
