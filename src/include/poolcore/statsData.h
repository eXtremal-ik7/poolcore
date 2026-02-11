#ifndef __STATS_DATA_H_
#define __STATS_DATA_H_

#include "kvdb.h"
#include "poolcore/poolCore.h"
#include "poolcore/rocksdbBase.h"
#include "poolcommon/datFile.h"
#include "poolcommon/serialize.h"
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <string>

std::string partByTime(time_t time);

struct StatsRecord;

// +file serialization
struct CStatsElement {
  uint64_t SharesNum = 0;
  UInt<256> SharesWork = UInt<256>::zero();
  TimeInterval Time;
  uint32_t PrimePOWTarget = -1U;
  std::vector<uint32_t> PrimePOWSharesNum;

  void reset();
  void merge(const CStatsElement &other);
  void merge(const StatsRecord &record);
  std::vector<CStatsElement> distributeToGrid(int64_t beginMs, int64_t endMs, int64_t gridIntervalMs) const;
};

struct CStats {
  std::string WorkerId;
  uint32_t ClientsNum = 0;
  uint32_t WorkersNum = 0;
  uint64_t AveragePower = 0;
  double SharesPerSecond = 0.0;
  UInt<256> SharesWork = UInt<256>::zero();
  Timestamp LastShareTime;
  Timestamp Time;
};

struct CStatsSeries {
  std::deque<CStatsElement> Recent;
  CStatsElement Current;
  Timestamp LastShareTime;

  void addShare(const UInt<256> &workValue, Timestamp time, unsigned primeChainLength, unsigned primePOWTarget, bool isPrimePOW);
  void merge(std::vector<CStatsElement> &cells);
  std::vector<CStatsElement> flush(int64_t beginMs, int64_t endMs, int64_t gridIntervalMs, Timestamp removeTimePoint, std::set<int64_t> &removedTimes);
  void calcAverageMetrics(const CCoinInfo &coinInfo, std::chrono::seconds calculateInterval, CStats &result) const;
};

// +file serialization
struct CStatsFileRecord {
  enum { CurrentRecordVersion = 1 };

  std::string Login;
  std::string WorkerId;
  CStatsElement Element;
};

// +file serialization
struct CStatsFileData {
  enum { CurrentRecordVersion = 1 };

  uint64_t LastShareId = 0;
  std::vector<CStatsFileRecord> Records;
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
  CStatsSeriesSingle(const std::string &cachePath) : CachePath_(cachePath) {}
  CStatsSeries& series() { return Series_; }
  const CStatsSeries& series() const { return Series_; }
  const std::string& cachePath() const { return CachePath_; }
  uint64_t lastShareId() const { return LastShareId_; }
  void setAccumulationBegin(Timestamp t) { AccumulationBegin_ = t; }

  void addShare(const UInt<256> &workValue, Timestamp time, unsigned primeChainLength, unsigned primePOWTarget, bool isPrimePOW) {
    Series_.addShare(workValue, time, primeChainLength, primePOWTarget, isPrimePOW);
  }

  void load(const std::filesystem::path &dbPath, const std::string &coinName);
  void rebuildDatFile(const std::filesystem::path &dbPath, int64_t gridEndMs);
  void flush(Timestamp timeLabel, int64_t gridIntervalMs, Timestamp removeTimePoint,
             uint64_t lastShareId, kvdb<rocksdbBase>::Batch *batch,
             std::set<int64_t> &modifiedTimes, std::set<int64_t> &removedTimes);
private:
  std::string CachePath_;
  CStatsSeries Series_;
  uint64_t LastShareId_ = 0;
  Timestamp AccumulationBegin_;
};

void removeDatFile(const std::filesystem::path &dbPath, const std::string &cachePath, int64_t gridEndMs);

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
  CStatsSeriesMap(const std::string &cachePath) : CachePath_(cachePath) {}
  std::map<std::string, CStatsSeries>& map() { return Map_; }
  const std::map<std::string, CStatsSeries>& map() const { return Map_; }
  const std::string& cachePath() const { return CachePath_; }
  uint64_t lastShareId() const { return LastShareId_; }
  void setAccumulationBegin(Timestamp t) { AccumulationBegin_ = t; }

  void addShare(const std::string &login, const std::string &workerId, const UInt<256> &workValue, Timestamp time, unsigned primeChainLength, unsigned primePOWTarget, bool isPrimePOW) {
    Map_[makeStatsKey(login, workerId)].addShare(workValue, time, primeChainLength, primePOWTarget, isPrimePOW);
  }

  void load(const std::filesystem::path &dbPath, const std::string &coinName);
  void rebuildDatFile(const std::filesystem::path &dbPath, int64_t gridEndMs);
  void flush(Timestamp timeLabel, int64_t gridIntervalMs, Timestamp removeTimePoint,
             uint64_t lastShareId, kvdb<rocksdbBase>::Batch *batch,
             std::set<int64_t> &modifiedTimes, std::set<int64_t> &removedTimes);
  void exportRecentStats(std::chrono::seconds keepTime, std::vector<CStatsExportData> &result) const;
private:
  std::string CachePath_;
  std::map<std::string, CStatsSeries> Map_;
  uint64_t LastShareId_ = 0;
  Timestamp AccumulationBegin_;
};

template<>
struct DbIo<CStatsElement> {
  static inline void serialize(xmstream &out, const CStatsElement &data) {
    DbIo<decltype(data.SharesNum)>::serialize(out, data.SharesNum);
    DbIo<decltype(data.SharesWork)>::serialize(out, data.SharesWork);
    DbIo<decltype(data.Time)>::serialize(out, data.Time);
    DbIo<decltype(data.PrimePOWTarget)>::serialize(out, data.PrimePOWTarget);
    DbIo<decltype(data.PrimePOWSharesNum)>::serialize(out, data.PrimePOWSharesNum);
  }

  static inline void unserialize(xmstream &in, CStatsElement &data) {
    DbIo<decltype(data.SharesNum)>::unserialize(in, data.SharesNum);
    DbIo<decltype(data.SharesWork)>::unserialize(in, data.SharesWork);
    DbIo<decltype(data.Time)>::unserialize(in, data.Time);
    DbIo<decltype(data.PrimePOWTarget)>::unserialize(in, data.PrimePOWTarget);
    DbIo<decltype(data.PrimePOWSharesNum)>::unserialize(in, data.PrimePOWSharesNum);
  }
};

template<>
struct DbIo<CStatsFileRecord> {
  static inline void serialize(xmstream &out, const CStatsFileRecord &data) {
    DbIo<uint32_t>::serialize(out, data.CurrentRecordVersion);
    DbIo<decltype(data.Login)>::serialize(out, data.Login);
    DbIo<decltype(data.WorkerId)>::serialize(out, data.WorkerId);
    DbIo<decltype(data.Element)>::serialize(out, data.Element);
  }

  static inline void unserialize(xmstream &in, CStatsFileRecord &data) {
    uint32_t version;
    DbIo<uint32_t>::unserialize(in, version);
    if (version == 1) {
      DbIo<decltype(data.Login)>::unserialize(in, data.Login);
      DbIo<decltype(data.WorkerId)>::unserialize(in, data.WorkerId);
      DbIo<decltype(data.Element)>::unserialize(in, data.Element);
    } else {
      in.seekEnd(0, true);
    }
  }
};

template<>
struct DbIo<CStatsFileData> {
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
