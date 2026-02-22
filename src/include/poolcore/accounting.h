#ifndef __ACCOUNTING_H_
#define __ACCOUNTING_H_

#include "accountingState.h"
#include "accountingApi.h"
#include "shareLog.h"
#include "statsData.h"
#include "poolcommon/multiCall.h"
#include "poolcommon/periodicTimer.h"
#include "poolcommon/taskHandler.h"
#include <optional>

class CPriceFetcher;

class AccountingDb {
private:
  using DefaultCb = std::function<void(const char*)>;
  using ManualPayoutCallback = std::function<void(bool)>;
  using QueryBalanceCallback = std::function<void(const UserBalanceInfo&)>;
  using QueryPPLNSPayoutsCallback = std::function<void(const std::vector<CPPLNSPayoutInfo>&)>;
  using QueryPPLNSAccCallback = std::function<void(const std::vector<CPPLNSPayoutAcc>&)>;
  using PoolLuckCallback = std::function<void(const std::vector<double>&)>;
  using QueryPPSStateCallback = std::function<void(const CPPSState&)>;

  struct FeePlanCacheKey {
    std::string FeePlanId;
    EMiningMode Mode;
    bool operator==(const FeePlanCacheKey &other) const = default;
  };

  struct FeePlanCacheKeyHash {
    size_t operator()(const FeePlanCacheKey &key) const {
      size_t h = std::hash<std::string>{}(key.FeePlanId);
      h ^= std::hash<unsigned>{}(static_cast<unsigned>(key.Mode)) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

private:
  asyncBase *Base_;
  const PoolBackendConfig &_cfg;
  CCoinInfo CoinInfo_;
  UserManager &UserManager_;
  CNetworkClientDispatcher &ClientDispatcher_;
  CPriceFetcher &PriceFetcher_;
  CFeeEstimationService *FeeEstimationService_ = nullptr;

  CAccountingState State_;

  kvdb<rocksdbBase> RoundsDb_;
  kvdb<rocksdbBase> _foundBlocksDb;
  kvdb<rocksdbBase> _poolBalanceDb;
  kvdb<rocksdbBase> _payoutDb;
  kvdb<rocksdbBase> PPLNSPayoutsDb;
  kvdb<rocksdbBase> PPSPayoutsDb;
  kvdb<rocksdbBase> PPSHistoryDb_;
  ShareLog<CUserWorkSummaryBatch> ShareLog_;
  CStatsSeriesMap UserStatsAcc_;

  std::unordered_map<std::string, UserSettingsRecord> UserSettings_;
  std::unordered_map<std::string, std::string> UserFeePlanIds_;
  std::unordered_map<FeePlanCacheKey, std::vector<::UserFeePair>, FeePlanCacheKeyHash> FeePlanCache_;

  CPeriodicTimer ShareLogFlushTimer_;
  CPeriodicTimer UserStatsFlushTimer_;
  CPeriodicTimer PPSPayoutTimer_;
  CPeriodicTimer InstantPayoutTimer_;
  CPeriodicTimer StateFlushTimer_;

  TaskHandlerCoroutine<AccountingDb> TaskHandler_;

  CPayoutProcessor PayoutProcessor_;
  CAccountingApi Api_;

  enum class ERoundConfirmationResult {
    ENotConfirmed,
    EOrphan,
    EConfirmed
  };

  void printRecentStatistic();
  void ppsPayout();
  void applyPPSCorrection(MiningRound &R);

public:
  AccountingDb(asyncBase *base,
               const PoolBackendConfig &config,
               const CCoinInfo &coinInfo,
               UserManager &userMgr,
               CNetworkClientDispatcher &clientDispatcher,
               CPriceFetcher &priceFetcher);

  void taskHandler();

  uint64_t lastAggregatedShareId() { return std::min(State_.SavedShareId, UserStatsAcc_.lastSavedMsgId()); }
  uint64_t lastKnownShareId() { return std::max(State_.lastAcceptedMsgId(), UserStatsAcc_.lastAcceptedMsgId()); }

  void start();
  void stop();

  bool applyReward(const std::string &address,
                   const UInt<384> &value,
                   EMiningMode mode,
                   const CRewardParams &rewardParams,
                   rocksdbBase::CBatch &stateBatch,
                   kvdb<rocksdbBase>::Batch &payoutHistoryBatch);

  bool hasDeferredReward() { return CoinInfo_.HasDagFile; }
  void calculatePPLNSPayments(MiningRound &R);
  CProcessedWorkSummary processWorkSummaryBatch(const CUserWorkSummaryBatch &batch);
  void onUserWorkSummary(const CUserWorkSummaryBatch &batch);
  void onBlockFound(const CBlockFoundData &block);
  void onUserSettingsUpdate(const UserSettingsRecord &settings);
  void onFeePlanUpdate(const std::string &feePlanId, EMiningMode mode, const std::vector<::UserFeePair> &feeRecord);
  void onFeePlanDelete(const std::string &feePlanId);
  void onUserFeePlanChange(const std::string &login, const std::string &feePlanId);
  ERoundConfirmationResult processRoundConfirmation(MiningRound &R, int64_t confirmations, const std::string &hash, rocksdbBase::CBatch &stateBatch);
  void checkBlockConfirmations();
  void checkBlockExtraInfo();

  CAccountingApi &api() { return Api_; }
  CPayoutProcessor &payoutProcessor() { return PayoutProcessor_; }

  std::list<PayoutDbRecord> &getPayoutsQueue() { return State_.PayoutQueue; }
  kvdb<rocksdbBase> &getFoundBlocksDb() { return _foundBlocksDb; }
  kvdb<rocksdbBase> &getPoolBalanceDb() { return _poolBalanceDb; }
  kvdb<rocksdbBase> &getPayoutDb() { return _payoutDb; }
  kvdb<rocksdbBase> &getPPLNSPayoutsDb() { return PPLNSPayoutsDb; }
  kvdb<rocksdbBase> &getPPSPayoutsDb() { return PPSPayoutsDb; }

  const std::map<std::string, UserBalanceRecord> &getUserBalanceMap() { return State_.BalanceMap; }

  // Asynchronous api
  void manualPayout(const std::string &user, DefaultCb callback) {
    TaskHandler_.push([user, callback](AccountingDb *a) { callback(a->api().manualPayout(user)); });
  }
  void queryFoundBlocks(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback) {
    TaskHandler_.push([heightFrom, hashFrom, count, callback](AccountingDb *a) { a->api().queryFoundBlocks(heightFrom, hashFrom, count, callback); });
  }
  void queryPPLNSPayouts(const std::string &user, int64_t timeFrom, const std::string &hashFrom, uint32_t count, QueryPPLNSPayoutsCallback callback) {
    TaskHandler_.push([user, timeFrom, hashFrom, count, callback](AccountingDb *a) { callback(a->api().queryPPLNSPayouts(user, timeFrom, hashFrom, count)); });
  }
  void queryPPLNSAcc(const std::string &user, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, QueryPPLNSAccCallback callback) {
    TaskHandler_.push([user, timeFrom, timeTo, groupByInterval, callback](AccountingDb *a) { callback(a->api().queryPPLNSPayoutsAcc(user, timeFrom, timeTo, groupByInterval)); });
  }
  void queryUserBalance(const std::string &user, QueryBalanceCallback callback) {
    TaskHandler_.push([user, callback](AccountingDb *a) { callback(a->api().queryBalance(user)); });
  }
  void poolLuck(std::vector<int64_t> &&intervals, PoolLuckCallback callback) {
    TaskHandler_.push([intervals = std::move(intervals), callback](AccountingDb *a) { callback(a->api().poolLuck(intervals)); });
  }
  void queryPPSState(QueryPPSStateCallback callback) {
    TaskHandler_.push([callback = std::move(callback)](AccountingDb *a) { callback(a->api().queryPPSState()); });
  }
  void updateBackendSettings(std::optional<CBackendSettings::PPS> pps, std::optional<CBackendSettings::Payouts> payouts, DefaultCb cb) {
    TaskHandler_.push([pps = std::move(pps), payouts = std::move(payouts), cb = std::move(cb)](AccountingDb *a) { cb(a->api().updateBackendSettings(pps, payouts)); });
  }

  CBackendSettings backendSettings() const {
    return State_.BackendSettings.load(std::memory_order_relaxed);
  }
  void setFeeEstimationService(CFeeEstimationService *service) { FeeEstimationService_ = service; }

  // Asynchronous multi calls
  static void queryUserBalanceMulti(AccountingDb **backends, size_t backendsNum, const std::string &user, std::function<void(const UserBalanceInfo*, size_t)> callback) {
    MultiCall<UserBalanceInfo> *context = new MultiCall<UserBalanceInfo>(backendsNum, callback);
    for (size_t i = 0; i < backendsNum; i++)
      backends[i]->queryUserBalance(user, context->generateCallback(i));
  }
};

#endif //__ACCOUNTING_H_
