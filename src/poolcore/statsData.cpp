#include "poolcore/statsData.h"
#include "poolcommon/debug.h"
#include "poolcommon/path.h"
#include "loguru.hpp"
#include <algorithm>
#include <cassert>

void CStats::merge(const CWorkSummary &data)
{
  SharesNum += data.SharesNum;
  SharesWork += data.SharesWork;
  PrimePOWTarget = std::min(PrimePOWTarget, data.PrimePOWTarget);
}

void CStats::mergeScaled(const CWorkSummary &data, double fraction)
{
  SharesNum += static_cast<uint64_t>(std::round(data.SharesNum * fraction));
  UInt<256> scaledWork = data.SharesWork;
  scaledWork.mulfp(fraction);
  SharesWork += scaledWork;
  PrimePOWTarget = std::min(PrimePOWTarget, data.PrimePOWTarget);
}

void CStatsSeries::addWorkSummary(const CWorkSummary &data, Timestamp time)
{
  Current.merge(data);
  LastShareTime = time;
}

void CStatsSeries::addBaseWork(uint64_t sharesNum, const UInt<256> &sharesWork, Timestamp time)
{
  Current.SharesNum += sharesNum;
  Current.SharesWork += sharesWork;
  LastShareTime = time;
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

    primePOWTarget = std::min(primePOWTarget, it->Data.PrimePOWTarget);

    if (it->Time.TimeBegin >= windowBegin) {
      sharesNum += it->Data.SharesNum;
      sharesWork += it->Data.SharesWork;
    } else {
      // Straddles boundary — clip proportionally
      int64_t totalMs = it->Time.TimeEnd.count() - it->Time.TimeBegin.count();
      int64_t overlapMs = it->Time.TimeEnd.count() - windowBegin.count();
      double fraction = static_cast<double>(overlapMs) / static_cast<double>(totalMs);
      sharesNum += static_cast<uint64_t>(std::round(it->Data.SharesNum * fraction));
      UInt<256> partialWork = it->Data.SharesWork;
      partialWork.mulfp(fraction);
      sharesWork += partialWork;
      break;
    }
  }

  int64_t intervalSeconds = std::chrono::duration_cast<std::chrono::seconds>(calculateInterval).count();
  if (isDebugStatistic())
    CLOG_F(1,
           "  * interval: {}; shares num: {}; shares work: {}",
           intervalSeconds,
           sharesNum,
           sharesWork.getDecimal());

  result.SharesPerSecond = (double)sharesNum / intervalSeconds;
  result.AveragePower = coinInfo.calculateAveragePower(sharesWork, intervalSeconds, primePOWTarget);
  result.SharesWork = sharesWork;
  result.LastShareTime = LastShareTime;
}

void CStatsSeries::flush(Timestamp begin,
                         Timestamp end,
                         std::string_view login,
                         std::string_view workerId,
                         Timestamp currentTime,
                         kvdb<rocksdbBase>::Batch *batch,
                         std::set<Timestamp> &modifiedTimes,
                         std::set<Timestamp> &removedTimes)
{
  Timestamp removeTimePoint = currentTime - KeepTime_;

  if (Current.SharesNum > 0 || !Current.SharesWork.isZero()) {
    if (begin > end || (end - begin) > MaxBatchTimeInterval) {
      Current.reset();
    } else {
      Timestamp firstCellEnd = (begin + std::chrono::milliseconds(1)).alignUp(GridInterval_);
      Timestamp lastCellEnd = end.alignUp(GridInterval_);

      auto recentIt = Recent.begin();
      for (Timestamp cellEnd = firstCellEnd; cellEnd <= lastCellEnd; cellEnd += GridInterval_) {
        Timestamp cellBegin = cellEnd - GridInterval_;

        double fraction = overlapFraction(begin, end, cellBegin, cellEnd);
        if (fraction <= 0.0)
          continue;

        // Compute contribution for this cell
        CWorkSummary contribution;
        if (fraction >= 1.0)
          contribution = Current;
        else
          contribution.mergeScaled(Current, fraction);

        // Write to DB regardless of age
        if (batch) {
          CWorkSummaryEntryWithTime record;
          record.UserId = login;
          record.WorkerId = workerId;
          record.Time = TimeInterval(cellBegin, cellEnd);
          record.Data = contribution;
          batch->merge(record);
        }

        // Skip adding to Recent if cell is too old
        if (cellEnd < removeTimePoint)
          continue;

        // Advance iterator to find or insert position
        while (recentIt != Recent.end() && recentIt->Time.TimeEnd < cellEnd)
          ++recentIt;

        // Merge contribution into Recent
        if (recentIt != Recent.end() && recentIt->Time.TimeEnd == cellEnd) {
          recentIt->Data.merge(contribution);
        } else {
          CWorkSummaryWithTime cell;
          cell.Time = TimeInterval(cellBegin, cellEnd);
          cell.Data = contribution;
          recentIt = Recent.insert(recentIt, cell);
        }

        modifiedTimes.insert(cellEnd);
      }

      Current.reset();
      if (!Recent.empty())
        LastShareTime = std::max(LastShareTime, Recent.back().Time.TimeEnd);
    }
  }

  while (!Recent.empty() && Recent.front().Time.TimeEnd < removeTimePoint) {
    removedTimes.insert(Recent.front().Time.TimeEnd);
    Recent.pop_front();
  }
}

static bool parseStatsCacheFile(const std::string &coinName, CDatFile &file, CStatsFileData &fileData)
{
  FileDescriptor fd;
  if (!fd.open(path_to_utf8(file.Path).c_str())) {
    CLOG_F(ERROR, "StatisticDb: can't open file {}", file.Path);
    return false;
  }

  size_t fileSize = fd.size();
  xmstream stream(fileSize);
  size_t bytesRead = fd.read(stream.reserve(fileSize), 0, fileSize);
  fd.close();
  if (bytesRead != fileSize) {
    CLOG_F(ERROR, "StatisticDb: can't read file {}", file.Path);
    return false;
  }

  stream.seekSet(0);
  DbIo<CStatsFileData>::unserialize(stream, fileData);

  if (stream.remaining() || stream.eof()) {
    CLOG_F(ERROR, "<{}> StatisticDb: corrupted file {}", coinName, file.Path);
    return false;
  }

  file.LastShareId = fileData.LastShareId;
  return true;
}

void CStatsSeriesSingle::load(const std::filesystem::path &dbPath, const std::string &coinName)
{
  std::deque<CDatFile> files;
  enumerateDatFiles(files, dbPath / CachePath_, 3, true);

  size_t loadedCount = 0;
  for (auto &file : files) {
    CStatsFileData fileData;
    if (!parseStatsCacheFile(coinName, file, fileData)) {
      std::filesystem::remove(file.Path);
      continue;
    }

    if (fileData.Records.size() != 1) {
      CLOG_F(ERROR,
             "<{}> Pool stats file has {} records, expected 1: {}",
             coinName,
             fileData.Records.size(),
             file.Path);
      std::filesystem::remove(file.Path);
      continue;
    }

    const auto &record = fileData.Records[0];
    if (!record.UserId.empty() || !record.WorkerId.empty()) {
      CLOG_F(ERROR,
             "<{}> Pool stats record has non-empty login/workerId: {}",
             coinName,
             file.Path);
      std::filesystem::remove(file.Path);
      continue;
    }

    int64_t gridIntervalMs = std::chrono::duration_cast<std::chrono::milliseconds>(GridInterval_).count();
    Timestamp gridEnd(file.FileId * 1000);
    Timestamp gridBegin = gridEnd - std::chrono::milliseconds(gridIntervalMs);
    CWorkSummaryWithTime entry;
    entry.Time = TimeInterval(gridBegin, gridEnd);
    entry.Data = record.Data;
    Series_.Recent.push_back(entry);
    Series_.LastShareTime = gridEnd;

    if (isDebugStatistic()) {
      CLOG_F(1, "<{}> Loaded pool stats: shares: {} work: {}",
             coinName,
             record.Data.SharesNum,
             record.Data.SharesWork.getDecimal());
    }

    LastSavedMsgId_ = std::max(LastSavedMsgId_, file.LastShareId);
    loadedCount++;
  }

  CLOG_F(INFO, "<{}> Loaded {} pool stats cache files from {}", coinName, loadedCount, CachePath_);
  LastAcceptedMsgId_ = LastSavedMsgId_;
}

void CStatsSeriesMap::load(const std::filesystem::path &dbPath, const std::string &coinName)
{
  std::deque<CDatFile> files;
  enumerateDatFiles(files, dbPath / CachePath_, 3, true);

  size_t loadedCount = 0;
  for (auto &file : files) {
    CStatsFileData fileData;
    if (!parseStatsCacheFile(coinName, file, fileData)) {
      std::filesystem::remove(file.Path);
      continue;
    }

    int64_t gridIntervalMs = std::chrono::duration_cast<std::chrono::milliseconds>(GridInterval_).count();
    Timestamp gridEnd(file.FileId * 1000);
    Timestamp gridBegin = gridEnd - std::chrono::milliseconds(gridIntervalMs);
    TimeInterval cellTime(gridBegin, gridEnd);

    auto hint = Map_.begin();
    for (const auto &record : fileData.Records) {
      auto key = makeStatsKey(record.UserId, record.WorkerId);
      hint = Map_.try_emplace(hint, std::move(key), GridInterval_, KeepTime_);
      auto &acc = hint->second;
      CWorkSummaryWithTime entry;
      entry.Time = cellTime;
      entry.Data = record.Data;
      acc.Recent.push_back(entry);
      acc.LastShareTime = gridEnd;

      if (isDebugStatistic()) {
        CLOG_F(1, "<{}> Loaded: {}/{} shares: {} work: {}",
               coinName,
               !record.UserId.empty() ? record.UserId : std::string("<empty>"),
               !record.WorkerId.empty() ? record.WorkerId : std::string("<empty>"),
               record.Data.SharesNum,
               record.Data.SharesWork.getDecimal());
      }
    }

    LastSavedMsgId_ = std::max(LastSavedMsgId_, file.LastShareId);
    loadedCount++;
  }

  CLOG_F(INFO, "<{}> Loaded {} stats cache files from {}", coinName, loadedCount, CachePath_);
  LastAcceptedMsgId_ = LastSavedMsgId_;
}

static void writeStatsRecord(const std::string &userId, const std::string &workerId, const CWorkSummary &data, xmstream &out)
{
  CWorkSummaryEntry record;
  record.UserId = userId;
  record.WorkerId = workerId;
  record.Data = data;
  DbIo<CWorkSummaryEntry>::serialize(out, record);
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
      DbIo<CStatsFileData>::serializeHeader(stream, LastSavedMsgId_, 1);
      writeStatsRecord("", "", it->Data, stream);

      auto filePath = datFilePath(dbPath, CachePath_, gridEndMs);
      FileDescriptor fd;
      if (!fd.open(filePath)) {
        CLOG_F(ERROR, "StatisticDb: can't write file {}", filePath);
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
    CLOG_F(1, "Removing old statistic cache file {}", filePath);
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
        writeStatsRecord(login, workerId, it->Data, recordsData);
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
  DbIo<CStatsFileData>::serializeHeader(header, LastSavedMsgId_, recordCount);

  auto filePath = datFilePath(dbPath, CachePath_, gridEndMs);
  FileDescriptor fd;
  if (!fd.open(filePath)) {
    CLOG_F(ERROR, "StatisticDb: can't write file {}", filePath);
    return;
  }
  fd.write(header.data(), header.sizeOf());
  fd.write(recordsData.data(), recordsData.sizeOf());
  fd.close();
}

void CStatsSeriesSingle::flush(Timestamp currentTime,
                               const std::filesystem::path &dbPath, kvdb<rocksdbBase> *db)
{
  LastSavedMsgId_ = LastAcceptedMsgId_;
  if (isDebugStatistic() && (Series_.Current.SharesNum > 0 || !Series_.Current.SharesWork.isZero()))
    CLOG_F(1, "Flush pool statistics (shares={}, work={})",
           Series_.Current.SharesNum,
           Series_.Current.SharesWork.getDecimal());

  kvdb<rocksdbBase>::Batch batch;
  std::set<Timestamp> modifiedTimes, removedTimes;
  Series_.flush(AccumulationInterval_.TimeBegin, AccumulationInterval_.TimeEnd,
                "", "", currentTime, db ? &batch : nullptr, modifiedTimes, removedTimes);

  if (db)
    db->writeBatch(batch);

  for (Timestamp t : removedTimes)
    modifiedTimes.erase(t);
  for (Timestamp t : modifiedTimes)
    rebuildDatFile(dbPath, t.count());
  for (Timestamp t : removedTimes)
    removeDatFile(dbPath, CachePath_, t.count());

  AccumulationInterval_ = emptyInterval();
}

void CStatsSeriesMap::flush(Timestamp currentTime,
                            const std::filesystem::path &dbPath, kvdb<rocksdbBase> *db)
{
  LastSavedMsgId_ = LastAcceptedMsgId_;

  kvdb<rocksdbBase>::Batch batch;
  std::set<Timestamp> modifiedTimes, removedTimes;
  for (auto it = Map_.begin(); it != Map_.end(); ) {
    auto [login, workerId] = splitStatsKey(it->first);

    if (isDebugStatistic() && (it->second.Current.SharesNum > 0 || !it->second.Current.SharesWork.isZero()))
      CLOG_F(1, "Flush statistics for {}/{} (shares={}, work={})",
             !login.empty() ? login : std::string("<none>"),
             !workerId.empty() ? workerId : std::string("<none>"),
             it->second.Current.SharesNum,
             it->second.Current.SharesWork.getDecimal());

    it->second.flush(AccumulationInterval_.TimeBegin, AccumulationInterval_.TimeEnd,
                     login, workerId, currentTime, db ? &batch : nullptr, modifiedTimes, removedTimes);

    if (it->second.Recent.empty())
      it = Map_.erase(it);
    else
      ++it;
  }

  if (db)
    db->writeBatch(batch);

  // Don't rebuild .dat files for times that will be removed anyway (data aged past KeepTime)
  for (Timestamp t : removedTimes)
    modifiedTimes.erase(t);
  for (Timestamp t : modifiedTimes)
    rebuildDatFile(dbPath, t.count());
  for (Timestamp t : removedTimes)
    removeDatFile(dbPath, CachePath_, t.count());

  AccumulationInterval_ = emptyInterval();
}

void CStatsSeriesMap::exportRecentStats(std::chrono::seconds window, std::vector<CStatsExportData> &result) const
{
  result.clear();
  Timestamp currentTime = Timestamp::now();
  for (const auto &[key, acc]: Map_) {
    // Skip users with no shares since last flush and no recent history in window
    Timestamp lastAcceptTime = currentTime - window;
    bool hasRecent = !acc.Recent.empty() && acc.Recent.back().Time.TimeEnd >= lastAcceptTime;
    if (acc.Current.SharesNum == 0 && acc.Current.SharesWork.isZero() && !hasRecent)
      continue;

    auto &userRecord = result.emplace_back();
    userRecord.UserId = splitStatsKey(key).first;
    // Current share work
    if (acc.Current.SharesNum > 0 || !acc.Current.SharesWork.isZero()) {
      auto &current = userRecord.Recent.emplace_back();
      current.SharesWork = acc.Current.SharesWork;
      current.Time.TimeEnd = currentTime;
      current.Time.TimeBegin = AccumulationInterval_.TimeBegin;
    }

    // Recent share work (up to N minutes)
    for (auto It = acc.Recent.rbegin(), ItE = acc.Recent.rend(); It != ItE; ++It) {
      if (It->Time.TimeEnd < lastAcceptTime)
        break;
      auto &recent = userRecord.Recent.emplace_back();
      recent.SharesWork = It->Data.SharesWork;
      recent.Time = It->Time;
    }
  }

  std::sort(result.begin(), result.end(), [](const CStatsExportData &l, const CStatsExportData &r) { return l.UserId < r.UserId; });
}

// ====================== CWorkSummaryEntryWithTime ======================

bool CWorkSummaryEntryWithTime::deserializeValue(xmstream &stream)
{
  uint32_t version;
  dbIoUnserialize(stream, version);
  if (version == 1) {
    dbIoUnserialize(stream, UserId);
    dbIoUnserialize(stream, WorkerId);
    dbIoUnserialize(stream, Time);
    DbIo<CWorkSummary>::unserialize(stream, Data);
  }

  return !stream.eof();
}

bool CWorkSummaryEntryWithTime::deserializeValue(const void *data, size_t size)
{
  xmstream stream((void*)data, size);
  return deserializeValue(stream);
}

void CWorkSummaryEntryWithTime::serializeKey(xmstream &stream) const
{
  dbKeyIoSerialize(stream, UserId);
  dbKeyIoSerialize(stream, WorkerId);
  DbKeyIo<Timestamp>::serialize(stream, Time.TimeEnd);
}

void CWorkSummaryEntryWithTime::serializeValue(xmstream &stream) const
{
  dbIoSerialize(stream, static_cast<uint32_t>(CurrentRecordVersion));
  dbIoSerialize(stream, UserId);
  dbIoSerialize(stream, WorkerId);
  dbIoSerialize(stream, Time);
  DbIo<CWorkSummary>::serialize(stream, Data);
}
