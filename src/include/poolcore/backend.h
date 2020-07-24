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
  using ManualPayoutCallback = std::function<void(bool)>;
  using QueryFoundBlocksCallback = std::function<void(const std::vector<FoundBlockRecord>&, const std::vector<CNetworkClient::GetBlockConfirmationsQuery>&)>;
  using QueryBalanceCallback = std::function<void(const UserBalanceRecord&)>;
  using QueryPoolStatsCallback = std::function<void(const SiteStatsRecord&)>;
  using QueryUserStatsCallback = std::function<void(const SiteStatsRecord&, const std::vector<ClientStatsRecord>&)>;

private:
  class Task {
  public:
    virtual ~Task() {}
    virtual void run(PoolBackend *backend) = 0;
  };

  class TaskShare : public Task {
  public:
    TaskShare(CAccountingShare *share) : Share_(share) {}
    void run(PoolBackend *backend) final { backend->onShare(Share_.get()); }
  private:
    std::unique_ptr<CAccountingShare> Share_;
  };

  class TaskStats : public Task {
  public:
    TaskStats(CUserStats *stats) : Stats_(stats) {}
    void run(PoolBackend *backend) final { backend->onStats(Stats_.get()); }
  private:
    std::unique_ptr<CUserStats> Stats_;
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
    TaskQueryUserStats(const std::string &user, QueryUserStatsCallback callback) : User_(user), Callback_(callback) {}
    void run(PoolBackend *backend) final { backend->queryUserStatsImpl(User_, Callback_); }
  private:
    std::string User_;
    QueryUserStatsCallback Callback_;
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
  
  void onShare(const CAccountingShare *share);
  void onStats(const CUserStats *stats);
  void manualPayoutImpl(const std::string &user, ManualPayoutCallback callback);
  void queryFoundBlocksImpl(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback);
  void queryBalanceImpl(const std::string &user, QueryBalanceCallback callback);
  void queryPoolStatsImpl(QueryPoolStatsCallback callback);
  void queryUserStatsImpl(const std::string &user, QueryUserStatsCallback callback);
  

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
  void queryPayouts(const std::string &user, uint64_t timeFrom, unsigned count, std::vector<PayoutDbRecord> &payouts);

  // Asynchronous api
  void sendShare(CAccountingShare *share) { startAsyncTask(new TaskShare(share)); }
  void sendStats(CUserStats *stats) { startAsyncTask(new TaskStats(stats)); }
  void manualPayout(const std::string &user, ManualPayoutCallback callback) { startAsyncTask(new TaskManualPayout(user, callback)); }
  void queryFoundBlocks(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback) { startAsyncTask(new TaskQueryFoundBlocks(heightFrom, hashFrom, count, callback)); }
  void queryUserBalance(const std::string &user, QueryBalanceCallback callback) { startAsyncTask(new TaskQueryBalance(user, callback)); }
  void queryPoolStats(QueryPoolStatsCallback callback) { startAsyncTask(new TaskQueryPoolStats(callback)); }
  void queryUserStats(const std::string &user, QueryUserStatsCallback callback) { startAsyncTask(new TaskQueryUserStats(user, callback)); }

  AccountingDb *accountingDb() { return _accounting.get(); }
  StatisticDb *statisticDb() { return _statistics.get(); }
};

#endif //__BACKEND_H_
