#ifndef __BACKEND_H_
#define __BACKEND_H_

#include "accounting.h"
#include "priceFetcher.h"
#include "shareLog.h"
#include "statistics.h"
#include "usermgr.h"
#include "asyncio/asyncio.h"
#include "asyncio/device.h"
#include <thread>
#include <tbb/concurrent_queue.h>


class ShareLogConfig {
public:
  ShareLogConfig() {}
  ShareLogConfig(AccountingDb *accounting, StatisticDb *statistic) : Accounting_(accounting), Statistic_(statistic) {}
  void initializationFinish(int64_t time) {
    Accounting_->initializationFinish(time);
    Statistic_->initializationFinish(time);
  }

  uint64_t minShareId() {
    return std::min(Statistic_->lastKnownShareId(), Accounting_->lastKnownShareId());
  }

  uint64_t maxShareId() {
    return std::max(Statistic_->lastKnownShareId(), Accounting_->lastKnownShareId());
  }

  void replayShare(const CShare &share) {
    Accounting_->replayShare(share);
    Statistic_->replayShare(share);
  }

private:
  AccountingDb *Accounting_;
  StatisticDb *Statistic_;
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
  ShareLog<ShareLogConfig> ShareLog_;

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

public:
  PoolBackend(const PoolBackend&) = delete;
  PoolBackend(PoolBackend&&) = default;
  PoolBackend(PoolBackendConfig &&cfg, const CCoinInfo &info, UserManager &userMgr, CNetworkClientDispatcher &clientDispatcher, CPriceFetcher &priceFetcher);

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

  AccountingDb *accountingDb() { return _accounting.get(); }
  StatisticDb *statisticDb() { return _statistics.get(); }
};

#endif //__BACKEND_H_
