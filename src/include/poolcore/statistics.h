#ifndef __STATISTICS_H_
#define __STATISTICS_H_

#include "kvdb.h"
#include "backendData.h"
#include "poolcore/poolCore.h"
#include "poolcore/rocksdbBase.h"
#include "poolcore/shareLog.h"
#include "poolcore/statsData.h"
#include "poolcore/usermgr.h"
#include "poolcommon/multiCall.h"
#include "poolcommon/taskHandler.h"
#include "poolcommon/timeTypes.h"
#include "asyncio/asyncio.h"

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
    Timestamp LastShareTime;

    enum EColumns {
      ELogin,
      EName,
      EEmail,
      ERegistrationDate,
      EWorkersNum,
      EAveragePower,
      ESharesPerSecond,
      ELastShareTime
    };
  };

private:
  using QueryPoolStatsCallback = std::function<void(const CStats&)>;
  using QueryUserStatsCallback = std::function<void(const CStats&, const std::vector<CStats>&)>;
  using QueryAllUsersStatisticCallback = std::function<void(const std::vector<CredentialsWithStatistic>&)>;

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
  const PoolBackendConfig _cfg;
  CCoinInfo CoinInfo_;
  uint64_t LastKnownShareId_ = 0;
  // Accumulators
  CStatsSeriesSingle PoolStatsAcc_;
  CStatsSeriesMap UserStats_;
  CStatsSeriesMap WorkerStats_;

  CStats PoolStatsCached_;

  kvdb<rocksdbBase> StatsDb_;
  ShareLog<CWorkSummaryBatch> ShareLog_;

  TaskHandlerCoroutine<StatisticDb> TaskHandler_;
  aioUserEvent *PoolFlushEvent_;
  aioUserEvent *UserFlushEvent_;
  aioUserEvent *WorkerFlushEvent_;
  aioUserEvent *ShareLogFlushEvent_ = nullptr;
  bool ShutdownRequested_ = false;
  bool PoolFlushFinished_ = false;
  bool UserFlushFinished_ = false;
  bool WorkerFlushFinished_ = false;
  bool ShareLogFlushFinished_ = false;

  // Debugging only
  struct {
    uint64_t MinShareId = std::numeric_limits<uint64_t>::max();
    uint64_t MaxShareId = 0;
    uint64_t Count = 0;
  } Dbg_;

  void flushPool(Timestamp timeLabel);
  void flushUsers(Timestamp timeLabel);
  void flushWorkers(Timestamp timeLabel);
  void updatePoolStatsCached(Timestamp timeLabel);
  void shareLogFlushHandler();

public:
  // Initialization
  StatisticDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo);
  void replayWorkSummary(uint64_t messageId, const CWorkSummaryBatch &batch);
  void start();
  void stop();
  const CCoinInfo &getCoinInfo() const { return CoinInfo_; }

  void onWorkSummary(const CWorkSummaryBatch &batch);

  // Min of accumulators' savedShareId; ShareLog replays shares starting from this point
  uint64_t lastAggregatedShareId() { return std::min({WorkerStats_.savedShareId(), UserStats_.savedShareId(), PoolStatsAcc_.savedShareId()}); }

  // Max share id ever seen; ShareLog uses it to continue UniqueShareId numbering after restart
  uint64_t lastKnownShareId() { return LastKnownShareId_; }

  // Not thread-safe — must only be called from the statistics event loop thread
  const CStats &getPoolStats() { return PoolStatsCached_; }
  void getUserStats(const std::string &user, CStats &aggregate, std::vector<CStats> &workerStats, size_t offset, size_t size, EStatsColumn sortBy, bool sortDescending);

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

  static void queryPoolStatsMulti(StatisticDb **backends, size_t backendsNum, std::function<void(const CStats*, size_t)> callback) {
    MultiCall<CStats> *context = new MultiCall<CStats>(backendsNum, callback);
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

class StatisticServer {
public:
  StatisticServer(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo);
  void start();
  void stop();
  StatisticDb *statisticDb() { return Statistics_.get(); }
  CCoinInfo &coinInfo() { return CoinInfo_; }

  // Asynchronous api
  void sendWorkSummary(CWorkSummaryBatch &&batch) { TaskHandler_.push(new TaskWorkSummary(std::move(batch))); }

private:
  class TaskWorkSummary : public Task<StatisticServer> {
  public:
    TaskWorkSummary(CWorkSummaryBatch &&batch) : Batch_(std::move(batch)) {}
    void run(StatisticServer *backend) final { backend->onWorkSummary(Batch_); }
  private:
    CWorkSummaryBatch Batch_;
  };

private:
  void onWorkSummary(const CWorkSummaryBatch &batch);
  void statisticServerMain();

private:
  asyncBase *Base_;
  const PoolBackendConfig Cfg_;
  CCoinInfo CoinInfo_;
  std::unique_ptr<StatisticDb> Statistics_;
  std::thread Thread_;
  TaskHandlerCoroutine<StatisticServer> TaskHandler_;
};


#endif //__STATISTICS_H_
