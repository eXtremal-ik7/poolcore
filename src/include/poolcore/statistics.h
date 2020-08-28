#ifndef __STATISTICS_H_
#define __STATISTICS_H_

#include "kvdb.h"
#include "backendData.h"
#include "poolcore/poolCore.h"
#include "poolcore/rocksdbBase.h"
#include <chrono>

struct CShare;

class StatisticDb {
public:
  struct CStats {
    std::string WorkerId;
    uint32_t ClientsNum = 0;
    uint32_t WorkersNum = 0;
    uint64_t AveragePower = 0;
    double SharesPerSecond = 0.0;
    double SharesWork = 0.0;
    int64_t LastShareTime = 0;
    int64_t Time = 0;
  };

  struct CStatsElement {
    uint32_t SharesNum = 0;
    double SharesWork = 0.0;
    int64_t TimeLabel = 0;
    void reset() {
      SharesNum = 0;
      SharesWork = 0.0;
    }
  };

  struct CStatsAccumulator {
    std::deque<CStatsElement> Recent;
    CStatsElement Current;
    int64_t LastShareTime = 0;
  };

private:
  struct CStatsFile {
    int64_t TimeLabel;
    uint64_t LastShareId;
    std::filesystem::path Path;
  };

  struct CFlushInfo {
    uint64_t ShareId;
    int64_t Time;
  };


private:
  asyncBase *Base_;
  const PoolBackendConfig &_cfg;
  CCoinInfo CoinInfo_;
  uint64_t LastKnownShareId_ = 0;
  // Pool stats
  CStats PoolStatsCached_;
  CStatsAccumulator PoolStatsAcc_;
  CFlushInfo PoolFlushInfo_;
  // Worker stats
  std::unordered_map<std::string, std::unordered_map<std::string, CStatsAccumulator>> LastWorkerStats_;
  CFlushInfo WorkersFlushInfo_;
  
  kvdb<rocksdbBase> WorkerStatsDb_;
  kvdb<rocksdbBase> PoolStatsDb_;
  std::deque<CStatsFile> PoolStatsCache_;
  std::deque<CStatsFile> WorkersStatsCache_;

  // Debugging only
  struct {
    uint64_t MinShareId = std::numeric_limits<uint64_t>::max();
    uint64_t MaxShareId = 0;
    uint64_t Count = 0;
  } Dbg_;
  
  static inline void parseStatsCacheFile(CStatsFile &file, std::function<CStatsAccumulator&(const StatsRecord&)> searchAcc);

  void enumerateStatsFiles(std::deque<CStatsFile> &cache, const std::filesystem::path &directory);
  void updateAcc(const std::string &login, const std::string &workerId, StatisticDb::CStatsAccumulator &acc, time_t currentTime, xmstream &statsFileData);
  void calcAverageMetrics(const StatisticDb::CStatsAccumulator &acc, std::chrono::seconds calculateInterval, std::chrono::seconds aggregateTime, CStats &result);
  void writeStatsToDb(const std::string &loginId, const std::string &workerId, const CStatsElement &element);
  void writeStatsToCache(const std::string &loginId, const std::string &workerId, const CStatsElement &element, int64_t lastShareTime, xmstream &statsFileData);

  void updateStatsDiskCache(const char *name, std::deque<CStatsFile> &cache, uint64_t timeLabel, uint64_t lastShareId, const void *data, size_t size);
  void updateWorkersStatsDiskCache(uint64_t timeLabel, uint64_t shareId, const void *data, size_t size) { updateStatsDiskCache("stats.workers.cache", WorkersStatsCache_, timeLabel, shareId, data, size); }
  void updatePoolStatsDiskCache(uint64_t timeLabel, uint64_t shareId, const void *data, size_t size) { updateStatsDiskCache("stats.pool.cache", PoolStatsCache_, timeLabel, shareId, data, size); }

public:
  // Initialization
  StatisticDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo);
  void replayShare(const CShare &share);
  void initializationFinish(int64_t timeLabel);
  void start();

  void addShare(const CShare &share);

  uint64_t lastAggregatedShareId() {
    uint64_t workersLastId = !WorkersStatsCache_.empty() ? WorkersStatsCache_.back().LastShareId : 0;
    uint64_t poolLastId = !PoolStatsCache_.empty() ? PoolStatsCache_.back().LastShareId : 0;
    return std::min(workersLastId, poolLastId);
  }

  uint64_t lastKnownShareId() { return LastKnownShareId_; }

  void updateWorkersStats(int64_t timeLabel);
  void updatePoolStats(int64_t timeLabel);
  
  const CStats &getPoolStats() { return PoolStatsCached_; }
  void getUserStats(const std::string &user, CStats &aggregate, std::vector<CStats> &workerStats);
  void getHistory(const std::string &login, const std::string &workerId, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, std::vector<CStats> &history);
};


#endif //__STATISTICS_H_
