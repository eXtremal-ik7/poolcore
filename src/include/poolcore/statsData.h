#ifndef __STATS_DATA_H_
#define __STATS_DATA_H_

#include "kvdb.h"
#include "poolcore/poolCore.h"
#include "poolcore/rocksdbBase.h"
#include "poolcore/workSummary.h"
#include "poolcommon/datFile.h"
#include "poolcommon/serialize.h"
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <string_view>

std::string partByTime(time_t time);

struct CStats {
  std::string WorkerId;
  uint32_t ClientsNum = 0;
  uint32_t WorkersNum = 0;
  uint64_t SharesNum = 0;
  uint64_t AveragePower = 0;
  double SharesPerSecond = 0.0;
  UInt<256> SharesWork = UInt<256>::zero();
  uint32_t PrimePOWTarget = -1U;
  Timestamp LastShareTime;
  Timestamp Time;

  void merge(const CWorkSummary &data);
  void mergeScaled(const CWorkSummary &data, double fraction);
};

struct CStatsSeries {
  CStatsSeries(std::chrono::minutes gridInterval, std::chrono::minutes keepTime)
    : GridInterval_(gridInterval), KeepTime_(keepTime) {}

  std::deque<CWorkSummaryWithTime> Recent;
  CWorkSummary Current;
  Timestamp LastShareTime;

  void addWorkSummary(const CWorkSummary &data, Timestamp time);
  void addBaseWork(uint64_t sharesNum, const UInt<256> &sharesWork, Timestamp time);
  // currentTime — used to compute removeTimePoint = currentTime - KeepTime_;
  // records with TimeEnd < removeTimePoint are removed from Recent (but still written to DB)
  void flush(Timestamp begin,
             Timestamp end,
             std::string_view login,
             std::string_view workerId,
             Timestamp currentTime,
             kvdb<rocksdbBase>::Batch *batch,
             std::set<Timestamp> &modifiedTimes,
             std::set<Timestamp> &removedTimes);
  void calcAverageMetrics(const CCoinInfo &coinInfo, std::chrono::seconds calculateInterval, Timestamp now, CStats &result) const;

private:
  std::chrono::minutes GridInterval_;
  std::chrono::minutes KeepTime_;
};

// +file serialization
struct CStatsFileData {
  enum { CurrentRecordVersion = 1 };

  uint64_t LastShareId = 0;
  std::vector<CWorkSummaryEntry> Records;
};

struct CWorkSummaryEntryWithTime {
  enum { CurrentRecordVersion = 1 };

  std::string UserId;
  std::string WorkerId;
  TimeInterval Time;
  CWorkSummary Data;

  std::string getPartitionId() const { return partByTime(Time.TimeEnd.toUnixTime()); }
  bool deserializeValue(xmstream &stream);
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct CStatsSeriesSingle {

public:
  CStatsSeriesSingle(const std::string &cachePath, std::chrono::minutes gridInterval, std::chrono::minutes keepTime)
    : CachePath_(cachePath), GridInterval_(gridInterval), KeepTime_(keepTime), Series_(gridInterval, keepTime) {}

  CStatsSeries& series() { return Series_; }
  const CStatsSeries& series() const { return Series_; }
  uint64_t lastSavedMsgId() const { return LastSavedMsgId_; }
  uint64_t lastAcceptedMsgId() const { return LastAcceptedMsgId_; }

  void addBatch(uint64_t msgId, const CWorkSummaryBatch &batch) {
    if (msgId <= LastAcceptedMsgId_)
      return;
    LastAcceptedMsgId_ = msgId;
    AccumulationInterval_.expand(batch.Time);
    for (const auto &entry : batch.Entries)
      Series_.Current.merge(entry.Data);
    if (!batch.Entries.empty())
      Series_.LastShareTime = batch.Time.TimeEnd;
  }

  void load(const std::filesystem::path &dbPath, const std::string &coinName);
  void flush(Timestamp currentTime, const std::filesystem::path &dbPath, kvdb<rocksdbBase> *db);

private:
  static TimeInterval emptyInterval() { return {Timestamp(std::chrono::milliseconds::max()), Timestamp()}; }
  void rebuildDatFile(const std::filesystem::path &dbPath, int64_t gridEndMs);

  std::string CachePath_;
  std::chrono::minutes GridInterval_;
  std::chrono::minutes KeepTime_;
  CStatsSeries Series_;
  uint64_t LastSavedMsgId_ = 0;
  uint64_t LastAcceptedMsgId_ = 0;
  TimeInterval AccumulationInterval_ = emptyInterval();
};

// All map keys must be created via makeStatsKey (guarantees '\0' separator)
inline std::string makeStatsKey(const std::string &login, const std::string &workerId) {
  return login + '\0' + workerId;
}

inline std::pair<std::string, std::string> splitStatsKey(const std::string &key) {
  size_t sep = key.find('\0');
  return {key.substr(0, sep), key.substr(sep + 1)};
}

struct CSharesWorkWithTime {
  TimeInterval Time;
  UInt<256> SharesWork;
};

// +file serialization
struct CStatsExportData {
  enum { CurrentRecordVersion = 1 };

  std::string UserId;
  std::vector<CSharesWorkWithTime> Recent;

  UInt<256> recentShareValue(Timestamp acceptSharesTime) const {
    UInt<256> shareValue = UInt<256>::zero();
    for (auto &statsElement: Recent) {
      if (statsElement.Time.TimeEnd > acceptSharesTime)
        shareValue += statsElement.SharesWork;
      else
        break;
    }

    return shareValue;
  }
};

struct CStatsSeriesMap {

public:
  CStatsSeriesMap(const std::string &cachePath, std::chrono::minutes gridInterval, std::chrono::minutes keepTime)
    : CachePath_(cachePath), GridInterval_(gridInterval), KeepTime_(keepTime) {}

  std::map<std::string, CStatsSeries>& map() { return Map_; }
  const std::map<std::string, CStatsSeries>& map() const { return Map_; }
  uint64_t lastSavedMsgId() const { return LastSavedMsgId_; }
  uint64_t lastAcceptedMsgId() const { return LastAcceptedMsgId_; }

  void addBatch(uint64_t msgId, const CWorkSummaryBatch &batch, bool useWorkerId = true) {
    if (msgId <= LastAcceptedMsgId_)
      return;
    LastAcceptedMsgId_ = msgId;
    AccumulationInterval_.expand(batch.Time);
    for (const auto &entry : batch.Entries)
      Map_.try_emplace(
        makeStatsKey(entry.UserId, useWorkerId ? entry.WorkerId : std::string()),
        GridInterval_,
        KeepTime_)
        .first->second.addWorkSummary(entry.Data, batch.Time.TimeEnd);
  }

  void addBaseWorkBatch(uint64_t msgId, const CUserWorkSummaryBatch &batch) {
    if (msgId <= LastAcceptedMsgId_)
      return;
    LastAcceptedMsgId_ = msgId;
    AccumulationInterval_.expand(batch.Time);
    for (const auto &entry : batch.Entries)
      Map_.try_emplace(makeStatsKey(entry.UserId, ""), GridInterval_, KeepTime_)
        .first->second.addBaseWork(entry.SharesNum, entry.AcceptedWork, batch.Time.TimeEnd);
  }

  void load(const std::filesystem::path &dbPath, const std::string &coinName);
  void flush(Timestamp currentTime, const std::filesystem::path &dbPath, kvdb<rocksdbBase> *db);
  void exportRecentStats(std::chrono::seconds window, std::vector<CStatsExportData> &result) const;

private:
  static TimeInterval emptyInterval() { return {Timestamp(std::chrono::milliseconds::max()), Timestamp()}; }
  void rebuildDatFile(const std::filesystem::path &dbPath, int64_t gridEndMs);

  std::string CachePath_;
  std::chrono::minutes GridInterval_;
  std::chrono::minutes KeepTime_;
  std::map<std::string, CStatsSeries> Map_;
  uint64_t LastSavedMsgId_ = 0;
  uint64_t LastAcceptedMsgId_ = 0;
  TimeInterval AccumulationInterval_ = emptyInterval();
};

template<>
struct DbIo<CStatsFileData> {
  static inline void serializeHeader(xmstream &out, uint64_t lastShareId, size_t recordCount) {
    DbIo<uint32_t>::serialize(out, CStatsFileData::CurrentRecordVersion);
    DbIo<uint64_t>::serialize(out, lastShareId);
    DbIo<VarSize>::serialize(out, VarSize(recordCount));
  }

  static inline void unserialize(xmstream &in, CStatsFileData &data) {
    uint32_t version;
    DbIo<uint32_t>::unserialize(in, version);
    if (version == 1) {
      DbIo<decltype(data.LastShareId)>::unserialize(in, data.LastShareId);
      DbIo<decltype(data.Records)>::unserialize(in, data.Records);
    } else {
      in.seekEnd(0, true);
    }
  }
};

template<>
struct DbIo<CSharesWorkWithTime> {
  static inline void serialize(xmstream &out, const CSharesWorkWithTime &data) {
    DbIo<decltype(data.SharesWork)>::serialize(out, data.SharesWork);
    DbIo<decltype(data.Time)>::serialize(out, data.Time);
  }

  static inline void unserialize(xmstream &in, CSharesWorkWithTime &data) {
    DbIo<decltype(data.SharesWork)>::unserialize(in, data.SharesWork);
    DbIo<decltype(data.Time)>::unserialize(in, data.Time);
  }
};

template<>
struct DbIo<CStatsExportData> {
  static inline void serialize(xmstream &out, const CStatsExportData &data) {
    DbIo<uint32_t>::serialize(out, data.CurrentRecordVersion);
    DbIo<decltype(data.UserId)>::serialize(out, data.UserId);
    DbIo<decltype(data.Recent)>::serialize(out, data.Recent);
  }

  static inline void unserialize(xmstream &in, CStatsExportData &data) {
    uint32_t version;
    DbIo<uint32_t>::unserialize(in, version);
    if (version == 1) {
      DbIo<decltype(data.UserId)>::unserialize(in, data.UserId);
      DbIo<decltype(data.Recent)>::unserialize(in, data.Recent);
    } else {
      in.seekEnd(0, true);
    }
  }
};

#endif //__STATS_DATA_H_
