#ifndef __BACKEND_H_
#define __BACKEND_H_

#include "accounting.h"
#include "blockTemplate.h"
#include "priceFetcher.h"
#include "statistics.h"
#include "usermgr.h"
#include "blockmaker/ethash.h"
#include "asyncio/asyncio.h"
#include "asyncio/device.h"
#include <thread>
#include <tbb/concurrent_queue.h>


class PoolBackend {
public:
  // Ethash DAG files number
  static constexpr size_t MaxEpochNum = 48000;

public:
  using ManualPayoutCallback = std::function<void(bool)>;
  using QueryFoundBlocksCallback = std::function<void(const std::vector<FoundBlockRecord>&, const std::vector<CNetworkClient::GetBlockConfirmationsQuery>&)>;
  using QueryBalanceCallback = std::function<void(const UserBalanceRecord&)>;
  using QueryPoolStatsCallback = std::function<void(const CStats&)>;
  using QueryUserStatsCallback = std::function<void(const CStats&, const std::vector<CStats>&)>;
  using QueryStatsHistoryCallback = std::function<void(const std::vector<CStats>&)>;

private:
  class TaskUpdateDag : public Task<PoolBackend> {
  public:
    TaskUpdateDag(unsigned epochNumber, bool bigEpoch) : EpochNumber_(epochNumber), BigEpoch_(bigEpoch) {}
    void run(PoolBackend *backend) final { backend->onUpdateDag(EpochNumber_, BigEpoch_); }
  private:
    unsigned EpochNumber_;
    bool BigEpoch_;
  };

  class TaskUserWorkSummary : public Task<PoolBackend> {
  public:
    TaskUserWorkSummary(CUserWorkSummaryBatch &&batch) : Batch_(std::move(batch)) {}
    void run(PoolBackend *backend) final { backend->onUserWorkSummary(Batch_); }
  private:
    CUserWorkSummaryBatch Batch_;
  };

  class TaskWorkSummary : public Task<PoolBackend> {
  public:
    TaskWorkSummary(CWorkSummaryBatch &&batch) : Batch_(std::move(batch)) {}
    void run(PoolBackend *backend) final { backend->onWorkSummary(Batch_); }
  private:
    CWorkSummaryBatch Batch_;
  };

  class TaskBlockFound : public Task<PoolBackend> {
  public:
    TaskBlockFound(CBlockFoundData *block) : Block_(block) {}
    void run(PoolBackend *backend) final { backend->onBlockFound(*Block_); }
  private:
    std::unique_ptr<CBlockFoundData> Block_;
  };

  class TaskUserSettingsUpdate : public Task<PoolBackend> {
  public:
    TaskUserSettingsUpdate(UserSettingsRecord settings) : Settings_(std::move(settings)) {}
    void run(PoolBackend *backend) final { backend->onUserSettingsUpdate(Settings_); }

  private:
    UserSettingsRecord Settings_;
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

  TaskHandlerCoroutine<PoolBackend> TaskHandler_;
  bool ShutdownRequested_ = false;
  bool CheckConfirmationsHandlerFinished_ = false;
  bool PayoutHandlerFinished_ = false;
  bool CheckBalanceHandlerFinished_ = false;
  aioUserEvent *CheckConfirmationsEvent_ = nullptr;
  aioUserEvent *PayoutEvent_ = nullptr;
  aioUserEvent *CheckBalanceEvent_ = nullptr;

  double ProfitSwitchCoeff_ = 0.0;

  atomic_intrusive_ptr<EthashDagWrapper> *EthDagFiles_ = nullptr;

  void backendMain();
  void checkConfirmationsHandler();
  void payoutHandler();
  void checkBalanceHandler();
  
  void onUpdateDag(unsigned epochNumber, bool bigEpoch);
  void onUserWorkSummary(const CUserWorkSummaryBatch &batch);
  void onWorkSummary(const CWorkSummaryBatch &batch);
  void onBlockFound(const CBlockFoundData &block);
  void onUserSettingsUpdate(const UserSettingsRecord &settings);

public:
  PoolBackend(const PoolBackend&) = delete;
  PoolBackend(PoolBackend&&) = delete;
  ~PoolBackend() { delete[] EthDagFiles_; }
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
  void updateDag(unsigned epochNumber, bool bigEpoch) { TaskHandler_.push(new TaskUpdateDag(epochNumber, bigEpoch)); }
  void sendUserWorkSummary(CUserWorkSummaryBatch &&batch) { TaskHandler_.push(new TaskUserWorkSummary(std::move(batch))); }
  void sendWorkSummary(CWorkSummaryBatch &&batch) { TaskHandler_.push(new TaskWorkSummary(std::move(batch))); }
  void sendBlockFound(CBlockFoundData *block) { TaskHandler_.push(new TaskBlockFound(block)); }
  void sendUserSettingsUpdate(UserSettingsRecord settings) { TaskHandler_.push(new TaskUserSettingsUpdate(std::move(settings))); }

  AccountingDb *accountingDb() { return _accounting.get(); }
  StatisticDb *statisticDb() { return _statistics.get(); }
};

#endif //__BACKEND_H_
