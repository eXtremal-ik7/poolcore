#ifndef __BACKEND_H_
#define __BACKEND_H_

#include "accounting.h"
#include "priceFetcher.h"
#include "statistics.h"
#include "usermgr.h"
#include "asyncio/asyncio.h"
#include "asyncio/device.h"
#include <thread>
#include <tbb/concurrent_queue.h>

template<typename T>
struct MultiCall {
  std::unique_ptr<T[]> Data;
  std::atomic<uint32_t> FinishedCallsNum = 0;
  uint32_t TotalCallsNum;
  std::function<void(const T*, size_t)> MainCallback;

  MultiCall(uint32_t totalCallsNum, std::function<void(const T*, size_t)> mainCallback) : TotalCallsNum(totalCallsNum), MainCallback(mainCallback) {
    Data.reset(new T[totalCallsNum]);
  }

  std::function<void(const T&)> generateCallback(uint32_t callNum) {
    return [this, callNum](const T &data) {
      Data[callNum] = data;
      if (++FinishedCallsNum == TotalCallsNum) {
        MainCallback(Data.get(), TotalCallsNum);
        delete this;
      }
    };
  }
};

template<typename T>
struct DefaultIo {
  static void serialize(xmstream &out, const T &data);
  static void unserialize(xmstream &in, T &data);
};

template<typename T>
struct ShareLogIo {
  static void serialize(xmstream &out, const T &data);
  static void unserialize(xmstream &in, T &data);
};

template<>
struct ShareLogIo<CShare> {
  static void serialize(xmstream &out, const CShare &data);
  static void unserialize(xmstream &in, CShare &data);
};

class PoolBackend {
public:
  using ManualPayoutCallback = std::function<void(bool)>;
  using QueryFoundBlocksCallback = std::function<void(const std::vector<FoundBlockRecord>&, const std::vector<CNetworkClient::GetBlockConfirmationsQuery>&)>;
  using QueryBalanceCallback = std::function<void(const UserBalanceRecord&)>;
  using QueryPoolStatsCallback = std::function<void(const StatisticDb::CStats&)>;
  using QueryUserStatsCallback = std::function<void(const StatisticDb::CStats&, const std::vector<StatisticDb::CStats>&)>;
  using QueryStatsHistoryCallback = std::function<void(const std::vector<StatisticDb::CStats>&)>;

private:
  struct CShareLogFile {
    std::filesystem::path Path;
    uint64_t FirstId;
    uint64_t LastId;
    FileDescriptor Fd;
  };

  class Task {
  public:
    virtual ~Task() {}
    virtual void run(PoolBackend *backend) = 0;
  };

  class TaskShare : public Task {
  public:
    TaskShare(CShare *share) : Share_(share) {}
    void run(PoolBackend *backend) final { backend->onShare(Share_.get()); }
  private:
    std::unique_ptr<CShare> Share_;
  };

  class TaskManualPayout : public Task {
  public:
    TaskManualPayout(const std::string &user, ManualPayoutCallback callback) : User_(user), Callback_(callback) {}
    void run(PoolBackend *backend) final { backend->manualPayoutImpl(User_, Callback_); }
  private:
    std::string User_;
    ManualPayoutCallback Callback_;
  };

  class TaskQueryFoundBlocks : public Task {
  public:
    TaskQueryFoundBlocks(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback) : HeightFrom_(heightFrom), HashFrom_(hashFrom), Count_(count), Callback_(callback) {}
    void run(PoolBackend *backend) final { backend->queryFoundBlocksImpl(HeightFrom_, HashFrom_, Count_, Callback_); }
  private:
    int64_t HeightFrom_;
    std::string HashFrom_;
    uint32_t Count_;
    QueryFoundBlocksCallback Callback_;
  };

  class TaskQueryBalance : public Task {
  public:
    TaskQueryBalance(const std::string &user, QueryBalanceCallback callback) : User_(user), Callback_(callback) {}
    void run(PoolBackend *backend) final { backend->queryBalanceImpl(User_, Callback_); }
  private:
    std::string User_;
    QueryBalanceCallback Callback_;
  };

  class TaskQueryPoolStats : public Task {
  public:
    TaskQueryPoolStats(QueryPoolStatsCallback callback) : Callback_(callback) {}
    void run(PoolBackend *backend) final { backend->queryPoolStatsImpl(Callback_); }
  private:
    QueryPoolStatsCallback Callback_;
  };

  class TaskQueryUserStats : public Task {
  public:
    TaskQueryUserStats(const std::string &user, QueryUserStatsCallback callback, size_t offset, size_t size, StatisticDb::EStatsColumn sortBy, bool sortDescending) :
      User_(user), Callback_(callback), Offset_(offset), Size_(size), SortBy_(sortBy), SortDescending_(sortDescending) {}
    void run(PoolBackend *backend) final { backend->queryUserStatsImpl(User_, Callback_, Offset_, Size_, SortBy_, SortDescending_); }
  private:
    std::string User_;
    QueryUserStatsCallback Callback_;
    size_t Offset_;
    size_t Size_;
    StatisticDb::EStatsColumn SortBy_;
    bool SortDescending_;
  };

  class TaskQueryStatsHistory : public Task {
  public:
    TaskQueryStatsHistory(const std::string &user, const std::string &workerId, uint64_t timeFrom, uint64_t timeTo, uint64_t groupByInterval, QueryStatsHistoryCallback callback) :
      User_(user), WorkerId_(workerId), TimeFrom_(timeFrom), TimeTo_(timeTo), GroupByInterval_(groupByInterval), Callback_(callback) {}
    void run(PoolBackend *backend) final { backend->queryStatsHistoryImpl(User_, WorkerId_, TimeFrom_, TimeTo_, GroupByInterval_, Callback_); }
  private:
    std::string User_;
    std::string WorkerId_;
    uint64_t TimeFrom_;
    uint64_t TimeTo_;
    uint64_t GroupByInterval_;
    QueryStatsHistoryCallback Callback_;
  };

private:
  asyncBase *_base;
  uint64_t _timeout;
  std::thread _thread;
  aioUserEvent *TaskQueueEvent_;
  
  PoolBackendConfig _cfg;
  CCoinInfo CoinInfo_;
  UserManager &UserMgr_;
  CNetworkClientDispatcher &ClientDispatcher_;
  CPriceFetcher &PriceFetcher_;
  std::unique_ptr<AccountingDb> _accounting;
  std::unique_ptr<StatisticDb> _statistics;
  tbb::concurrent_queue<Task*> TaskQueue_;

  xmstream ShareLogInMemory_;
  std::deque<CShareLogFile> ShareLog_;
  uint64_t CurrentShareId_ = 0;
  bool ShareLoggingEnabled_ = true;

  double ProfitSwitchCoeff_ = 0.0;

  void startAsyncTask(Task *task) {
    TaskQueue_.push(task);
    userEventActivate(TaskQueueEvent_);
  }
  
  void backendMain();
  void shareLogFlush();
  void shareLogFlushHandler();
  void taskHandler();
  void *msgHandler();
  void *checkConfirmationsHandler();  
  void *payoutHandler();    
  void *checkBalanceHandler();
  
  void onShare(CShare *share);
  void manualPayoutImpl(const std::string &user, ManualPayoutCallback callback);
  void queryFoundBlocksImpl(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback);
  void queryBalanceImpl(const std::string &user, QueryBalanceCallback callback);
  void queryPoolStatsImpl(QueryPoolStatsCallback callback);
  void queryUserStatsImpl(const std::string &user, QueryUserStatsCallback callback, size_t offset, size_t size, StatisticDb::EStatsColumn sortBy, bool sortDescending);
  void queryStatsHistoryImpl(const std::string &user, const std::string &worker, uint64_t timeFrom, uint64_t timeTo, uint64_t groupByInteval, QueryStatsHistoryCallback callback);
  

public:
  PoolBackend(const PoolBackend&) = delete;
  PoolBackend(PoolBackend&&) = default;
  PoolBackend(PoolBackendConfig &&cfg, const CCoinInfo &info, UserManager &userMgr, CNetworkClientDispatcher &clientDispatcher, CPriceFetcher &priceFetcher);
  void startNewShareLogFile();
  void replayShares(CShareLogFile &file);

  const PoolBackendConfig &getConfig() const { return _cfg; }
  const CCoinInfo &getCoinInfo() const { return CoinInfo_; }
  CNetworkClientDispatcher &getClientDispatcher() const { return ClientDispatcher_; }
  CPriceFetcher &getPriceFetcher() const { return PriceFetcher_; }
  double getProfitSwitchCoeff() const { return ProfitSwitchCoeff_; }
  void setProfitSwitchCoeff(double profitSwithCoeff) { ProfitSwitchCoeff_ = profitSwithCoeff; }

  void start();
  void stop();

  // Synchronous api
  void queryPayouts(const std::string &user, uint64_t timeFrom, unsigned count, std::vector<PayoutDbRecord> &payouts);

  // Asynchronous api
  void sendShare(CShare *share) { startAsyncTask(new TaskShare(share)); }
  void manualPayout(const std::string &user, ManualPayoutCallback callback) { startAsyncTask(new TaskManualPayout(user, callback)); }

  void queryFoundBlocks(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback) { startAsyncTask(new TaskQueryFoundBlocks(heightFrom, hashFrom, count, callback)); }
  void queryUserBalance(const std::string &user, QueryBalanceCallback callback) { startAsyncTask(new TaskQueryBalance(user, callback)); }
  void queryPoolStats(QueryPoolStatsCallback callback) { startAsyncTask(new TaskQueryPoolStats(callback)); }
  void queryUserStats(const std::string &user, QueryUserStatsCallback callback, size_t offset, size_t size, StatisticDb::EStatsColumn sortBy, bool sortDescending) {
    startAsyncTask(new TaskQueryUserStats(user, callback, offset, size, sortBy, sortDescending));
  }

  void queryStatsHistory(const std::string &user,
                         const std::string &worker,
                         uint64_t timeFrom,
                         uint64_t timeTo,
                         uint64_t groupByInterval,
                         QueryStatsHistoryCallback callback) {
    startAsyncTask(new TaskQueryStatsHistory(user, worker, timeFrom, timeTo, groupByInterval, callback));
  }

  // Asynchronous multi calls
  static void queryUserBalanceMulti(PoolBackend **backends, size_t backendsNum, const std::string &user, std::function<void(const UserBalanceRecord*, size_t)> callback) {
    MultiCall<UserBalanceRecord> *context = new MultiCall<UserBalanceRecord>(backendsNum, callback);
    for (size_t i = 0; i < backendsNum; i++)
      backends[i]->queryUserBalance(user, context->generateCallback(i));
  }

  static void queryPoolStatsMulti(PoolBackend **backends, size_t backendsNum, std::function<void(const StatisticDb::CStats*, size_t)> callback) {
    MultiCall<StatisticDb::CStats> *context = new MultiCall<StatisticDb::CStats>(backendsNum, callback);
    for (size_t i = 0; i < backendsNum; i++)
      backends[i]->queryPoolStats(context->generateCallback(i));
  }

  AccountingDb *accountingDb() { return _accounting.get(); }
  StatisticDb *statisticDb() { return _statistics.get(); }
};

#endif //__BACKEND_H_
