#ifndef __BACKEND_H_
#define __BACKEND_H_

#include "accounting.h"
#include "statistics.h"
#include "usermgr.h"
#include "asyncio/asyncio.h"
#include "asyncio/device.h"
#include <thread>
#include <tbb/concurrent_queue.h>

class p2pNode;

class PoolBackend {
private:
  class Task {
  public:
    virtual ~Task() {}
    virtual void run(PoolBackend *backend) = 0;
  };

  class TaskShare : public Task {
  public:
    TaskShare(Share *share) : Share_(share) {}
    void run(PoolBackend *backend) final { backend->onShare(Share_.get()); }
  private:
    std::unique_ptr<Share> Share_;
  };

  class TaskStats : public Task {
  public:
    TaskStats(Stats *stats) : Stats_(stats) {}
    void run(PoolBackend *backend) final { backend->onStats(Stats_.get()); }
  private:
    std::unique_ptr<Stats> Stats_;
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

  AccountingDb *accountingDb() { return _accounting.get(); }
  StatisticDb *statisticDb() { return _statistics.get(); }
};

#endif //__BACKEND_H_
