#ifndef __BACKEND_H_
#define __BACKEND_H_

#include "accounting.h"
#include "blockTemplate.h"
#include "feeEstimator.h"
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
  asyncBase *_base;
  uint64_t _timeout;
  std::thread _thread;
  
  PoolBackendConfig _cfg;
  CCoinInfo CoinInfo_;
  UserManager &UserMgr_;
  CNetworkClientDispatcher &ClientDispatcher_;
  std::unique_ptr<CFeeEstimationService> FeeEstimationService_;
  std::unique_ptr<AccountingDb> _accounting;
  std::unique_ptr<StatisticDb> _statistics;
  StatisticServer *AlgoMetaStatistic_ = nullptr;

  TaskHandlerCoroutine<PoolBackend> TaskHandler_;
  CPeriodicTimer CheckConfirmationsTimer_;
  CPeriodicTimer CheckBalanceTimer_;

  double ProfitSwitchCoeff_ = 0.0;

  atomic_intrusive_ptr<EthashDagWrapper> *EthDagFiles_ = nullptr;

  void backendMain();
  
  void onUpdateDag(unsigned epochNumber, bool bigEpoch);
  void onUserWorkSummary(const CUserWorkSummaryBatch &batch);
  void onWorkSummary(const CWorkSummaryBatch &batch);
  void onBlockFound(const CBlockFoundData &block, const std::vector<PoolBackend*> &shareBackends);
  void onUserSettingsUpdate(const UserSettingsRecord &settings);
  void onFeePlanUpdate(const std::string &feePlanId, EMiningMode mode, const std::vector<UserFeePair> &feeRecord);
  void onFeePlanDelete(const std::string &feePlanId);
  void onUserFeePlanChange(const std::string &login, const std::string &feePlanId);

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
  CFeeEstimationService *feeEstimationService() { return FeeEstimationService_.get(); }
  double getProfitSwitchCoeff() const { return ProfitSwitchCoeff_; }
  void setProfitSwitchCoeff(double profitSwithCoeff) { ProfitSwitchCoeff_ = profitSwithCoeff; }
  StatisticServer *getAlgoMetaStatistic() { return AlgoMetaStatistic_; }
  void setAlgoMetaStatistic(StatisticServer *server) { AlgoMetaStatistic_ = server; }

  void start();
  void stop();

  intrusive_ptr<EthashDagWrapper> dagFile(unsigned epochNumber) { return epochNumber < MaxEpochNum ? EthDagFiles_[epochNumber] : intrusive_ptr<EthashDagWrapper>(); }

  // Synchronous api
  void queryPayouts(const std::string &user, Timestamp timeFrom, unsigned count, std::vector<PayoutDbRecord> &payouts);

  // Asynchronous api
  void updateDag(unsigned epochNumber, bool bigEpoch) {
    TaskHandler_.push([epochNumber, bigEpoch](PoolBackend *b) { b->onUpdateDag(epochNumber, bigEpoch); });
  }
  void sendUserWorkSummary(CUserWorkSummaryBatch &&batch) {
    TaskHandler_.push([batch = std::move(batch)](PoolBackend *b) { b->onUserWorkSummary(batch); });
  }
  void sendWorkSummary(CWorkSummaryBatch &&batch) {
    TaskHandler_.push([batch = std::move(batch)](PoolBackend *b) { b->onWorkSummary(batch); });
  }
  void sendBlockFound(CBlockFoundData *block, std::vector<PoolBackend*> shareBackends) {
    TaskHandler_.push([block = std::unique_ptr<CBlockFoundData>(block),
                       shareBackends = std::move(shareBackends)](PoolBackend *b) {
      b->onBlockFound(*block, shareBackends);
    });
  }
  void sendMergedBlockNotification(std::string coinName, uint64_t height, std::string hash, BaseBlob<256> shareHash) {
    TaskHandler_.push([coinName = std::move(coinName), height, hash = std::move(hash), shareHash](PoolBackend *b) {
      b->_accounting->onMergedBlockNotification(coinName, height, hash, shareHash);
    });
  }
  void sendUserSettingsUpdate(UserSettingsRecord settings) {
    TaskHandler_.push([settings = std::move(settings)](PoolBackend *b) { b->onUserSettingsUpdate(settings); });
  }
  void sendFeePlanUpdate(std::string feePlanId, EMiningMode mode, std::vector<UserFeePair> feeRecord) {
    TaskHandler_.push([feePlanId = std::move(feePlanId), mode, feeRecord = std::move(feeRecord)](PoolBackend *b) { b->onFeePlanUpdate(feePlanId, mode, feeRecord); });
  }
  void sendFeePlanDelete(std::string feePlanId) {
    TaskHandler_.push([feePlanId = std::move(feePlanId)](PoolBackend *b) { b->onFeePlanDelete(feePlanId); });
  }
  void sendUserFeePlanChange(std::string login, std::string feePlanId) {
    TaskHandler_.push([login = std::move(login), feePlanId = std::move(feePlanId)](PoolBackend *b) { b->onUserFeePlanChange(login, feePlanId); });
  }

  AccountingDb *accountingDb() { return _accounting.get(); }
  StatisticDb *statisticDb() { return _statistics.get(); }
};

#endif //__BACKEND_H_
