#ifndef __BACKEND_H_
#define __BACKEND_H_

#include "accounting.h"
#include "blockTemplate.h"
#include "priceFetcher.h"
#include "shareLog.h"
#include "statistics.h"
#include "usermgr.h"
#include "blockmaker/ethash.h"
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

  uint64_t lastAggregatedShareId() {
    return std::min(Statistic_->lastAggregatedShareId(), Accounting_->lastAggregatedShareId());
  }

  uint64_t lastKnownShareId() {
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
  // Ethash DAG files number
  static constexpr size_t MaxEpochNum = 48000;

public:
  using ManualPayoutCallback = std::function<void(bool)>;
  using QueryFoundBlocksCallback = std::function<void(const std::vector<FoundBlockRecord>&, const std::vector<CNetworkClient::GetBlockConfirmationsQuery>&)>;
  using QueryBalanceCallback = std::function<void(const UserBalanceRecord&)>;
  using QueryPoolStatsCallback = std::function<void(const StatisticDb::CStats&)>;
  using QueryUserStatsCallback = std::function<void(const StatisticDb::CStats&, const std::vector<StatisticDb::CStats>&)>;
  using QueryStatsHistoryCallback = std::function<void(const std::vector<StatisticDb::CStats>&)>;

private:
  class TaskShare : public Task<PoolBackend> {
  public:
    TaskShare(CShare *share) : Share_(share) {}
    void run(PoolBackend *backend) final { backend->onShare(Share_.get()); }
  private:
    std::unique_ptr<CShare> Share_;
  };

  class TaskUpdateDag : public Task<PoolBackend> {
  public:
    TaskUpdateDag(unsigned epochNumber, bool bigEpoch) : EpochNumber_(epochNumber), BigEpoch_(bigEpoch) {}
    void run(PoolBackend *backend) final { backend->onUpdateDag(EpochNumber_, BigEpoch_); }
  private:
    unsigned EpochNumber_;
    bool BigEpoch_;
  };

private:
  asyncBase *_base;
  uint64_t _timeout;
  std::thread _thread;
  
  PoolBackendConfig _cfg;
  CCoinInfo CoinInfo_;
  UserManager &UserMgr_;
  CNetworkClientDispatcher &ClientDispatcher_;
  std::unique_ptr<AccountingDb> _accounting;
  std::unique_ptr<StatisticDb> _statistics;
  StatisticServer *AlgoMetaStatistic_ = nullptr;
  ShareLog<ShareLogConfig> ShareLog_;

  TaskHandlerCoroutine<PoolBackend> TaskHandler_;
  bool ShutdownRequested_ = false;
  bool CheckConfirmationsHandlerFinished_ = false;
  bool PayoutHandlerFinished_ = false;
  bool CheckBalanceHandlerFinished_ = false;
  aioUserEvent *CheckConfirmationsEvent_ = nullptr;
  aioUserEvent *PayoutEvent_ = nullptr;
  aioUserEvent *CheckBalanceEvent_ = nullptr;

  double ProfitSwitchCoeff_ = 0.0;

  atomic_intrusive_ptr<EthashDagWrapper> *EthDagFiles_;

  void backendMain();
  void checkConfirmationsHandler();
  void payoutHandler();
  void checkBalanceHandler();
  
  void onShare(CShare *share);
  void onUpdateDag(unsigned epochNumber, bool bigEpoch);

public:
  PoolBackend(const PoolBackend&) = delete;
  PoolBackend(PoolBackend&&) = default;
  PoolBackend(asyncBase *base,
              const PoolBackendConfig &cfg,
              const CCoinInfo &info,
              UserManager &userMgr,
              CNetworkClientDispatcher &clientDispatcher,
              CPriceFetcher &priceFetcher);

  const PoolBackendConfig &getConfig() const { return _cfg; }
  const CCoinInfo &getCoinInfo() const { return CoinInfo_; }
  CNetworkClientDispatcher &getClientDispatcher() const { return ClientDispatcher_; }
  double getProfitSwitchCoeff() const { return ProfitSwitchCoeff_; }
  void setProfitSwitchCoeff(double profitSwithCoeff) { ProfitSwitchCoeff_ = profitSwithCoeff; }
  StatisticServer *getAlgoMetaStatistic() { return AlgoMetaStatistic_; }
  void setAlgoMetaStatistic(StatisticServer *server) { AlgoMetaStatistic_ = server; }

  void start();
  void stop();

  intrusive_ptr<EthashDagWrapper> dagFile(unsigned epochNumber) { return epochNumber < MaxEpochNum ? EthDagFiles_[epochNumber] : intrusive_ptr<EthashDagWrapper>(); }

  // Synchronous api
  void queryPayouts(const std::string &user, uint64_t timeFrom, unsigned count, std::vector<PayoutDbRecord> &payouts);

  // Asynchronous api
  void sendShare(CShare *share) { TaskHandler_.push(new TaskShare(share)); }
  void updateDag(unsigned epochNumber, bool bigEpoch) { TaskHandler_.push(new TaskUpdateDag(epochNumber, bigEpoch)); }

  AccountingDb *accountingDb() { return _accounting.get(); }
  StatisticDb *statisticDb() { return _statistics.get(); }
};

#endif //__BACKEND_H_
