#ifndef __BACKEND_H_
#define __BACKEND_H_

#include "accounting.h"
#include "statistics.h"
#include "usermgr.h"
#include "asyncio/asyncio.h"
#include "asyncio/device.h"
#include <thread>
#include <tbb/concurrent_queue.h>

class PoolBackend {
public:
  using QueryFoundBlocksCallback = std::function<void(const std::vector<FoundBlockRecord>&, const std::vector<CNetworkClient::GetBlockConfirmationsQuery>&)>;

private:
  class Task {
  public:
    virtual ~Task() {}
    virtual void run(PoolBackend *backend) = 0;
  };

  class TaskShare : public Task {
  public:
    TaskShare(Share *share) : Share_(share) {}
    virtual ~TaskShare() {}
    void run(PoolBackend *backend) final { backend->onShare(Share_.get()); }
  private:
    std::unique_ptr<Share> Share_;
  };

  class TaskStats : public Task {
  public:
    TaskStats(Stats *stats) : Stats_(stats) {}
    virtual ~TaskStats() {}
    void run(PoolBackend *backend) final { backend->onStats(Stats_.get()); }
  private:
    std::unique_ptr<Stats> Stats_;
  };

  class QueryFoundBlocksTask : public Task {
  public:
    QueryFoundBlocksTask(uint64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback) : HeightFrom_(heightFrom), HashFrom_(hashFrom), Count_(count), Callback_(callback) {}
    virtual ~QueryFoundBlocksTask() {}
    void run(PoolBackend *backend) final { backend->queryFoundBlocksImpl(HeightFrom_, HashFrom_, Count_, Callback_); }
  private:
    uint64_t HeightFrom_;
    std::string HashFrom_;
    uint32_t Count_;
    QueryFoundBlocksCallback Callback_;
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
  std::unique_ptr<AccountingDb> _accounting;
  std::unique_ptr<StatisticDb> _statistics;
  tbb::concurrent_queue<Task*> TaskQueue_;

  void startAsyncTask(Task *task) {
    TaskQueue_.push(task);
    userEventActivate(TaskQueueEvent_);
  }

  static void checkConfirmationsProc(void *arg) { ((PoolBackend*)arg)->checkConfirmationsHandler(); }
  static void payoutProc(void *arg) { ((PoolBackend*)arg)->payoutHandler(); }
  static void checkBalanceProc(void *arg) { ((PoolBackend*)arg)->checkBalanceHandler(); }
  static void updateStatisticProc(void *arg) { ((PoolBackend*)arg)->updateStatisticHandler(); }
  
  void backendMain();
  void taskHandler();
  void *msgHandler();
  void *checkConfirmationsHandler();  
  void *payoutHandler();    
  void *checkBalanceHandler();
  void *updateStatisticHandler();  
  
  void onShare(const Share *share);
  void onStats(const Stats *stats);
  void queryFoundBlocksImpl(uint64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback);
  

public:
  PoolBackend(const PoolBackend&) = delete;
  PoolBackend(PoolBackend&&) = default;
  PoolBackend(PoolBackendConfig &&cfg, const CCoinInfo &info, UserManager &userMgr, CNetworkClientDispatcher &clientDispatcher);
  const PoolBackendConfig &getConfig() { return _cfg; }
  const CCoinInfo &getCoinInfo() { return CoinInfo_; }
  CNetworkClientDispatcher &getClientDispatcher() { return ClientDispatcher_; }

  void start();
  void stop();

  // Synchronous api

  // Asynchronous api
  void sendShare(Share *share) { startAsyncTask(new TaskShare(share)); }
  void sendStats(Stats *stats) { startAsyncTask(new TaskStats(stats)); }
  void queryFoundBlocks(uint64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback) { startAsyncTask(new QueryFoundBlocksTask(heightFrom, hashFrom, count, callback)); }

  AccountingDb *accountingDb() { return _accounting.get(); }
  StatisticDb *statisticDb() { return _statistics.get(); }
};

#endif //__BACKEND_H_
