#ifndef __STATISTICS_H_
#define __STATISTICS_H_

#include "kvdb.h"
#include "backendData.h"
#include "poolcore/poolCore.h"
#include "poolcore/rocksdbBase.h"
#include "poolcore/shareLog.h"
#include "poolcore/usermgr.h"
#include "poolcommon/multiCall.h"
#include "poolcommon/serialize.h"
#include "poolcommon/taskHandler.h"
#include "asyncio/asyncio.h"
#include <tbb/concurrent_queue.h>
#include <chrono>

struct CShare;

class StatisticDb {
public:
  enum EStatsColumn {
    EStatsColumnName = 0,
    EStatsColumnAveragePower,
    EStatsColumnSharesPerSecond,
    EStatsColumnLastShareTime
  };

  struct CredentialsWithStatistic {
    UserManager::Credentials Credentials;
    uint32_t WorkersNum = 0;
    uint64_t AveragePower = 0;
    double SharesPerSecond = 0.0;
    int64_t LastShareTime = 0;

    enum EColumns {
      ELogin,
      EName,
      EEmail,
      ERegistrationDate,
      EWorkersNum,
      EAveragePower,
      ESharesPerSecord,
      ELastShareTime
    };
  };

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
    std::vector<uint32_t> PrimePOWSharesNum;
    void reset() {
      SharesNum = 0;
      SharesWork = 0.0;
      PrimePOWSharesNum.clear();
    }


  };

  struct CStatsAccumulator {
    std::deque<CStatsElement> Recent;
    CStatsElement Current;
    int64_t LastShareTime = 0;

    void addShare(double workValue, int64_t time, unsigned chainLength, bool isPrimePOW) {
      Current.SharesNum++;
      Current.SharesWork += workValue;
      if (isPrimePOW) {
        chainLength = std::min(chainLength, 1024u);
        if (chainLength >= Current.PrimePOWSharesNum.size())
          Current.PrimePOWSharesNum.resize(chainLength + 1);
        Current.PrimePOWSharesNum[chainLength]++;
      }
      LastShareTime = time;
    }
  };

  struct CStatsExportData {
    std::string UserId;
    std::vector<CStatsElement> Recent;

    double recentShareValue(int64_t acceptSharesTime) const {
      double shareValue = 0.0;
      for (auto &statsElement: Recent) {
        if (statsElement.TimeLabel > acceptSharesTime)
          shareValue += statsElement.SharesWork;
        else
          break;
      }

      return shareValue;
    }
  };

private:
  using QueryPoolStatsCallback = std::function<void(const StatisticDb::CStats&)>;
  using QueryUserStatsCallback = std::function<void(const StatisticDb::CStats&, const std::vector<StatisticDb::CStats>&)>;
  using QueryStatsHistoryCallback = std::function<void(const std::vector<StatisticDb::CStats>&)>;
  using QueryAllUsersStatisticCallback = std::function<void(const std::vector<CredentialsWithStatistic>&)>;

  struct CStatsFile {
    int64_t TimeLabel;
    uint64_t LastShareId;
    std::filesystem::path Path;
  };

  struct CFlushInfo {
    uint64_t ShareId;
    int64_t Time;
  };

  class TaskQueryPoolStats : public Task<StatisticDb> {
  public:
    TaskQueryPoolStats(QueryPoolStatsCallback callback) : Callback_(callback) {}
    void run(StatisticDb *statistic) final { statistic->queryPoolStatsImpl(Callback_); }
  private:
    QueryPoolStatsCallback Callback_;
  };

  class TaskQueryUserStats : public Task<StatisticDb> {
  public:
    TaskQueryUserStats(const std::string &user, QueryUserStatsCallback callback, size_t offset, size_t size, StatisticDb::EStatsColumn sortBy, bool sortDescending) :
      User_(user), Callback_(callback), Offset_(offset), Size_(size), SortBy_(sortBy), SortDescending_(sortDescending) {}
    void run(StatisticDb *statistic) final { statistic->queryUserStatsImpl(User_, Callback_, Offset_, Size_, SortBy_, SortDescending_); }
  private:
    std::string User_;
    QueryUserStatsCallback Callback_;
    size_t Offset_;
    size_t Size_;
    StatisticDb::EStatsColumn SortBy_;
    bool SortDescending_;
  };

  class TaskQueryAllUsersStats : public Task<StatisticDb> {
  public:
    TaskQueryAllUsersStats(std::vector<UserManager::Credentials> &&users, QueryAllUsersStatisticCallback callback, size_t offset, size_t size, CredentialsWithStatistic::EColumns sortBy, bool sortDescending) :
      Users_(std::move(users)), Callback_(callback), Offset_(offset), Size_(size), SortBy_(sortBy), SortDescending_(sortDescending) {}
    void run(StatisticDb *statistic) final { statistic->queryAllUserStatsImpl(Users_, Callback_, Offset_, Size_, SortBy_, SortDescending_); }
  private:
    std::vector<UserManager::Credentials> Users_;
    QueryAllUsersStatisticCallback Callback_;
    size_t Offset_;
    size_t Size_;
    CredentialsWithStatistic::EColumns SortBy_;
    bool SortDescending_;
  };

private:
  asyncBase *Base_;
  const PoolBackendConfig _cfg;
  CCoinInfo CoinInfo_;
  uint64_t LastKnownShareId_ = 0;
  // Pool stats
  CStats PoolStatsCached_;
  CStatsAccumulator PoolStatsAcc_;
  CFlushInfo PoolFlushInfo_;
  // Worker stats
  std::unordered_map<std::string, std::unordered_map<std::string, CStatsAccumulator>> LastWorkerStats_;
  std::unordered_map<std::string, CStatsAccumulator> LastUserStats_;
  CFlushInfo WorkersFlushInfo_;

  kvdb<rocksdbBase> WorkerStatsDb_;
  kvdb<rocksdbBase> PoolStatsDb_;
  std::deque<CStatsFile> PoolStatsCache_;
  std::deque<CStatsFile> WorkersStatsCache_;

  TaskHandlerCoroutine<StatisticDb> TaskHandler_;
  aioUserEvent *WorkerStatsUpdaterEvent_;
  aioUserEvent *PoolStatsUpdaterEvent_;

  bool ShutdownRequested_ = false;
  bool WorkerStatsUpdaterFinished_ = false;
  bool PoolStatsUpdaterFinished_ = false;

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

  void updateStatsDiskCache(const char *name, std::deque<CStatsFile> &cache, int64_t timeLabel, uint64_t lastShareId, const void *data, size_t size);
  void updateWorkersStatsDiskCache(uint64_t timeLabel, uint64_t shareId, const void *data, size_t size) { updateStatsDiskCache("stats.workers.cache.2", WorkersStatsCache_, timeLabel, shareId, data, size); }
  void updatePoolStatsDiskCache(uint64_t timeLabel, uint64_t shareId, const void *data, size_t size) { updateStatsDiskCache("stats.pool.cache.2", PoolStatsCache_, timeLabel, shareId, data, size); }

public:
  // Initialization
  StatisticDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo);
  void replayShare(const CShare &share);
  void initializationFinish(int64_t timeLabel);
  void start();
  void stop();
  const CCoinInfo &getCoinInfo() const { return CoinInfo_; }

  void addShare(const CShare &share, bool updateWorkerAndUserStats, bool updatePoolStats);

  uint64_t lastAggregatedShareId() {
    uint64_t workersLastId = !WorkersStatsCache_.empty() ? WorkersStatsCache_.back().LastShareId : 0;
    uint64_t poolLastId = !PoolStatsCache_.empty() ? PoolStatsCache_.back().LastShareId : 0;
    return std::min(workersLastId, poolLastId);
  }

  uint64_t lastKnownShareId() { return LastKnownShareId_; }

  void updateWorkersStats(int64_t timeLabel);
  void updatePoolStats(int64_t timeLabel);

  const CStats &getPoolStats() { return PoolStatsCached_; }
  void getUserStats(const std::string &user, CStats &aggregate, std::vector<CStats> &workerStats, size_t offset, size_t size, EStatsColumn sortBy, bool sortDescending);

  /// Return recent statistic for users
  /// result - sorted by UserId
  void exportRecentStats(std::vector<CStatsExportData> &result);

  // Synchronous api
  void getHistory(const std::string &login, const std::string &workerId, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, std::vector<CStats> &history);

  // Asynchronous api
  void queryPoolStats(QueryPoolStatsCallback callback) { TaskHandler_.push(new TaskQueryPoolStats(callback)); }
  void queryUserStats(const std::string &user, QueryUserStatsCallback callback, size_t offset, size_t size, StatisticDb::EStatsColumn sortBy, bool sortDescending) {
    TaskHandler_.push(new TaskQueryUserStats(user, callback, offset, size, sortBy, sortDescending));
  }

  void queryAllusersStats(std::vector<UserManager::Credentials> &&users,
                          QueryAllUsersStatisticCallback callback,
                          size_t offset,
                          size_t size,
                          CredentialsWithStatistic::EColumns sortBy,
                          bool sortDescending) {
    TaskHandler_.push(new TaskQueryAllUsersStats(std::move(users), callback, offset, size, sortBy, sortDescending));
  }

  static void queryPoolStatsMulti(StatisticDb **backends, size_t backendsNum, std::function<void(const StatisticDb::CStats*, size_t)> callback) {
    MultiCall<StatisticDb::CStats> *context = new MultiCall<StatisticDb::CStats>(backendsNum, callback);
    for (size_t i = 0; i < backendsNum; i++)
      backends[i]->queryPoolStats(context->generateCallback(i));
  }

private:
  void queryPoolStatsImpl(QueryPoolStatsCallback callback);
  void queryUserStatsImpl(const std::string &user, QueryUserStatsCallback callback, size_t offset, size_t size, StatisticDb::EStatsColumn sortBy, bool sortDescending);

  void queryAllUserStatsImpl(const std::vector<UserManager::Credentials> &users,
                             QueryAllUsersStatisticCallback callback,
                             size_t offset,
                             size_t size,
                             CredentialsWithStatistic::EColumns sortBy,
                             bool sortDescending);
};

template<>
struct DbIo<StatisticDb::CStatsElement> {
  static inline void serialize(xmstream &out, const StatisticDb::CStatsElement &data) {
    DbIo<decltype(data.SharesNum)>::serialize(out, data.SharesNum);
    DbIo<decltype(data.SharesWork)>::serialize(out, data.SharesWork);
    DbIo<decltype(data.TimeLabel)>::serialize(out, data.TimeLabel);
    DbIo<decltype(data.PrimePOWSharesNum)>::serialize(out, data.PrimePOWSharesNum);
  }

  static inline void unserialize(xmstream &in, StatisticDb::CStatsElement &data) {
    DbIo<decltype(data.SharesNum)>::unserialize(in, data.SharesNum);
    DbIo<decltype(data.SharesWork)>::unserialize(in, data.SharesWork);
    DbIo<decltype(data.TimeLabel)>::unserialize(in, data.TimeLabel);
    DbIo<decltype(data.PrimePOWSharesNum)>::unserialize(in, data.PrimePOWSharesNum);
  }
};

template<>
struct DbIo<StatisticDb::CStatsExportData> {
  static inline void serialize(xmstream &out, const StatisticDb::CStatsExportData &data) {
    DbIo<decltype(data.UserId)>::serialize(out, data.UserId);
    DbIo<decltype(data.Recent)>::serialize(out, data.Recent);
  }

  static inline void unserialize(xmstream &in, StatisticDb::CStatsExportData &data) {
    DbIo<decltype(data.UserId)>::unserialize(in, data.UserId);
    DbIo<decltype(data.Recent)>::unserialize(in, data.Recent);
  }
};

class StatisticShareLogConfig {
public:
  StatisticShareLogConfig() {}
  StatisticShareLogConfig(StatisticDb *statistic) : Statistic_(statistic) {}
  void initializationFinish(int64_t time) { Statistic_->initializationFinish(time); }
  uint64_t lastAggregatedShareId() { return Statistic_->lastAggregatedShareId(); }
  uint64_t lastKnownShareId() { return Statistic_->lastKnownShareId(); }
  void replayShare(const CShare &share) { Statistic_->replayShare(share); }

private:
  StatisticDb *Statistic_;
};

class StatisticServer {
public:
  StatisticServer(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo);
  void start();
  void stop();
  StatisticDb *statisticDb() { return Statistics_.get(); }
  CCoinInfo &coinInfo() { return CoinInfo_; }

  // Asynchronous api
  void sendShare(CShare *share) { TaskHandler_.push(new TaskShare(share)); }

private:
  class TaskShare : public Task<StatisticServer> {
  public:
    TaskShare(CShare *share) : Share_(share) {}
    void run(StatisticServer *backend) final { backend->onShare(Share_.get()); }
  private:
    std::unique_ptr<CShare> Share_;
  };

private:
  void onShare(CShare *share);
  void statisticServerMain();

private:
  asyncBase *Base_;
  const PoolBackendConfig Cfg_;
  CCoinInfo CoinInfo_;
  std::unique_ptr<StatisticDb> Statistics_;
  ShareLog<StatisticShareLogConfig> ShareLog_;
  std::thread Thread_;
  TaskHandlerCoroutine<StatisticServer> TaskHandler_;
};


#endif //__STATISTICS_H_
