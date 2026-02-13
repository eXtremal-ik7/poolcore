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

struct StatsRecord;

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

  void merge(const StatsRecord &record);
  void mergeScaled(const StatsRecord &record, double fraction);
};

struct CStatsSeries {
  std::deque<CWorkSummary> Recent;
  CWorkSummary Current;
  Timestamp LastShareTime;

  void addWorkSummary(const CWorkSummary &data, Timestamp time);
  void addBaseWork(uint64_t sharesNum, const UInt<256> &sharesWork, Timestamp time);
  void merge(std::vector<CWorkSummary> &cells);
  void flush(int64_t beginMs,
             int64_t endMs,
             int64_t gridIntervalMs,
             Timestamp removeTimePoint,
             std::string_view login,
             std::string_view workerId,
             Timestamp timeLabel,
             kvdb<rocksdbBase>::Batch *batch,
             std::set<int64_t> &modifiedTimes,
             std::set<int64_t> &removedTimes);
  void calcAverageMetrics(const CCoinInfo &coinInfo, std::chrono::seconds calculateInterval, Timestamp now, CStats &result) const;
};

// +file serialization
struct CStatsFileData {
  enum { CurrentRecordVersion = 1 };

  uint64_t LastShareId = 0;
  std::vector<CWorkSummaryEntry> Records;
};

struct StatsRecord {
  enum { CurrentRecordVersion = 1 };

  std::string Login;
  std::string WorkerId;
  TimeInterval Time;
  Timestamp UpdateTime;
  uint64_t ShareCount = 0;
  UInt<256> ShareWork = UInt<256>::zero();
  uint32_t PrimePOWTarget = -1U;
  std::vector<uint64_t> PrimePOWShareCount;

  std::string getPartitionId() const { return partByTime(Time.TimeEnd.toUnixTime()); }
  bool deserializeValue(xmstream &stream);
  bool deserializeValue(const void *data, size_t size);
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
};

struct CStatsSeriesSingle {
  CStatsSeriesSingle(const std::string &cachePath, std::chrono::minutes gridInterval, std::chrono::minutes keepTime)
    : CachePath_(cachePath), GridInterval_(gridInterval), KeepTime_(keepTime) {}
  CStatsSeries& series() { return Series_; }
  const CStatsSeries& series() const { return Series_; }
  uint64_t savedShareId() const { return SavedShareId_; }
  void setAccumulationBegin(Timestamp t) { AccumulationBegin_ = t; }

  void addWorkSummary(const CWorkSummary &data, Timestamp time) {
    Series_.addWorkSummary(data, time);
  }

  void load(const std::filesystem::path &dbPath, const std::string &coinName);
  void flush(Timestamp timeLabel, uint64_t lastShareId,
             const std::filesystem::path &dbPath, kvdb<rocksdbBase> *db);
private:
  void rebuildDatFile(const std::filesystem::path &dbPath, int64_t gridEndMs);

  std::string CachePath_;
  std::chrono::minutes GridInterval_;
  std::chrono::minutes KeepTime_;
  CStatsSeries Series_;
  uint64_t SavedShareId_ = 0;
  Timestamp AccumulationBegin_;
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
  CStatsSeriesMap(const std::string &cachePath, std::chrono::minutes gridInterval, std::chrono::minutes keepTime)
    : CachePath_(cachePath), GridInterval_(gridInterval), KeepTime_(keepTime) {}
  std::map<std::string, CStatsSeries>& map() { return Map_; }
  const std::map<std::string, CStatsSeries>& map() const { return Map_; }
  uint64_t savedShareId() const { return SavedShareId_; }
  void setAccumulationBegin(Timestamp t) { AccumulationBegin_ = t; }

  void addWorkSummary(const std::string &login, const std::string &workerId, const CWorkSummary &data, Timestamp time) {
    Map_[makeStatsKey(login, workerId)].addWorkSummary(data, time);
  }

  void addBaseWork(const std::string &login, const std::string &workerId, uint64_t sharesNum, const UInt<256> &sharesWork, Timestamp time) {
    Map_[makeStatsKey(login, workerId)].addBaseWork(sharesNum, sharesWork, time);
  }

  void load(const std::filesystem::path &dbPath, const std::string &coinName);
  void flush(Timestamp timeLabel, uint64_t lastShareId,
             const std::filesystem::path &dbPath, kvdb<rocksdbBase> *db);
  void exportRecentStats(std::chrono::seconds window, std::vector<CStatsExportData> &result) const;
private:
  void rebuildDatFile(const std::filesystem::path &dbPath, int64_t gridEndMs);

  std::string CachePath_;
  std::chrono::minutes GridInterval_;
  std::chrono::minutes KeepTime_;
  std::map<std::string, CStatsSeries> Map_;
  uint64_t SavedShareId_ = 0;
  Timestamp AccumulationBegin_;
};

template<>
struct DbIo<CWorkSummary> {
  static inline void serialize(xmstream &out, const CWorkSummary &data) {
    DbIo<decltype(data.SharesNum)>::serialize(out, data.SharesNum);
    DbIo<decltype(data.SharesWork)>::serialize(out, data.SharesWork);
    DbIo<decltype(data.Time)>::serialize(out, data.Time);
    DbIo<decltype(data.PrimePOWTarget)>::serialize(out, data.PrimePOWTarget);
    DbIo<decltype(data.PrimePOWSharesNum)>::serialize(out, data.PrimePOWSharesNum);
  }

  static inline void unserialize(xmstream &in, CWorkSummary &data) {
    DbIo<decltype(data.SharesNum)>::unserialize(in, data.SharesNum);
    DbIo<decltype(data.SharesWork)>::unserialize(in, data.SharesWork);
    DbIo<decltype(data.Time)>::unserialize(in, data.Time);
    DbIo<decltype(data.PrimePOWTarget)>::unserialize(in, data.PrimePOWTarget);
    DbIo<decltype(data.PrimePOWSharesNum)>::unserialize(in, data.PrimePOWSharesNum);
  }
};

template<>
struct DbIo<CWorkSummaryEntry> {
  static inline void serialize(xmstream &out, const CWorkSummaryEntry &data) {
    DbIo<uint32_t>::serialize(out, data.CurrentRecordVersion);
    DbIo<decltype(data.UserId)>::serialize(out, data.UserId);
    DbIo<decltype(data.WorkerId)>::serialize(out, data.WorkerId);
    DbIo<decltype(data.Data)>::serialize(out, data.Data);
  }

  static inline void unserialize(xmstream &in, CWorkSummaryEntry &data) {
    uint32_t version;
    DbIo<uint32_t>::unserialize(in, version);
    if (version == 1) {
      DbIo<decltype(data.UserId)>::unserialize(in, data.UserId);
      DbIo<decltype(data.WorkerId)>::unserialize(in, data.WorkerId);
      DbIo<decltype(data.Data)>::unserialize(in, data.Data);
    } else {
      // Unknown version — skip the rest of the file; remaining records are
      // discarded intentionally since we cannot parse them reliably
      in.seekEnd(0, true);
    }
  }
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
