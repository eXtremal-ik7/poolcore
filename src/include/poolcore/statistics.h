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

private:
  const PoolBackendConfig _cfg;
  CCoinInfo CoinInfo_;
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

  void flushPool(Timestamp currentTime);
  void flushUsers(Timestamp currentTime);
  void flushWorkers(Timestamp currentTime);
  void updatePoolStatsCached(Timestamp currentTime);
  void shareLogFlushHandler();

public:
  // Initialization
  StatisticDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo);
  void start();
  void stop();
  const CCoinInfo &getCoinInfo() const { return CoinInfo_; }

  void onWorkSummary(const CWorkSummaryBatch &batch);

  // Min of accumulators' lastSavedMsgId; ShareLog replays shares starting from this point
  uint64_t lastAggregatedShareId() {
    return std::min({WorkerStats_.lastSavedMsgId(), UserStats_.lastSavedMsgId(), PoolStatsAcc_.lastSavedMsgId()});
  }

  // Max share id ever seen; ShareLog uses it to continue UniqueShareId numbering after restart
  uint64_t lastKnownShareId() {
    return std::max({WorkerStats_.lastAcceptedMsgId(), UserStats_.lastAcceptedMsgId(), PoolStatsAcc_.lastAcceptedMsgId()});
  }

  // Not thread-safe — must only be called from the statistics event loop thread
  const CStats &getPoolStats() { return PoolStatsCached_; }
  void getUserStats(const std::string &user, CStats &aggregate, std::vector<CStats> &workerStats, size_t offset, size_t size, EStatsColumn sortBy, bool sortDescending);

  // Synchronous api
  void getHistory(const std::string &login, const std::string &workerId, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, std::vector<CStats> &history);

  // Asynchronous api
  void queryPoolStats(QueryPoolStatsCallback callback) {
    TaskHandler_.push([callback](StatisticDb *s) { s->queryPoolStatsImpl(callback); });
  }
  void queryUserStats(const std::string &user, QueryUserStatsCallback callback, size_t offset, size_t size, StatisticDb::EStatsColumn sortBy, bool sortDescending) {
    TaskHandler_.push([user, callback, offset, size, sortBy, sortDescending](StatisticDb *s) { s->queryUserStatsImpl(user, callback, offset, size, sortBy, sortDescending); });
  }
  void queryAllusersStats(std::vector<UserManager::Credentials> &&users,
                          QueryAllUsersStatisticCallback callback,
                          size_t offset,
                          size_t size,
                          CredentialsWithStatistic::EColumns sortBy,
                          bool sortDescending) {
    TaskHandler_.push([users = std::move(users), callback, offset, size, sortBy, sortDescending](StatisticDb *s) { s->queryAllUserStatsImpl(users, callback, offset, size, sortBy, sortDescending); });
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
  void sendWorkSummary(CWorkSummaryBatch &&batch) {
    TaskHandler_.push([batch = std::move(batch)](StatisticServer *s) { s->onWorkSummary(batch); });
  }

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
