#ifndef __ACCOUNTING_H_
#define __ACCOUNTING_H_

#include "accountingState.h"
#include "accountingApi.h"
#include "shareLog.h"
#include "statsData.h"
#include "poolcommon/multiCall.h"
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

  class TaskManualPayout : public Task<AccountingDb> {
  public:
    TaskManualPayout(const std::string &user, DefaultCb callback) : User_(user), Callback_(callback) {}
    void run(AccountingDb *accounting) final {
      const char *status = accounting->api().manualPayout(User_);
      Callback_(status);
    }
  private:
    std::string User_;
    DefaultCb Callback_;
  };

  class TaskQueryFoundBlocks : public Task<AccountingDb> {
  public:
    TaskQueryFoundBlocks(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback) : HeightFrom_(heightFrom), HashFrom_(hashFrom), Count_(count), Callback_(callback) {}
    void run(AccountingDb *accounting) final { accounting->api().queryFoundBlocks(HeightFrom_, HashFrom_, Count_, Callback_); }
  private:
    int64_t HeightFrom_;
    std::string HashFrom_;
    uint32_t Count_;
    QueryFoundBlocksCallback Callback_;
  };

  class TaskQueryBalance : public Task<AccountingDb> {
  public:
    TaskQueryBalance(const std::string &user, QueryBalanceCallback callback) : User_(user), Callback_(callback) {}
    void run(AccountingDb *accounting) final { Callback_(accounting->api().queryBalance(User_)); }
  private:
    std::string User_;
    QueryBalanceCallback Callback_;
  };

  class TaskQueryPPLNSPayouts : public Task<AccountingDb> {
  public:
    TaskQueryPPLNSPayouts(const std::string &user, int64_t timeFrom, const std::string &hashFrom, uint32_t count, QueryPPLNSPayoutsCallback callback) :
      User_(user), TimeFrom_(timeFrom), HashFrom_(hashFrom), Count_(count), Callback_(callback) {}
    void run(AccountingDb *accounting) final { Callback_(accounting->api().queryPPLNSPayouts(User_, TimeFrom_, HashFrom_, Count_)); }
  private:
    std::string User_;
    int64_t TimeFrom_;
    std::string HashFrom_;
    uint32_t Count_;
    QueryPPLNSPayoutsCallback Callback_;
  };

  class TaskQueryPPLNSAcc: public Task<AccountingDb> {
  public:
    TaskQueryPPLNSAcc(const std::string &user, int64_t timeFrom, int64_t timeTo, int64_t groupInterval, QueryPPLNSAccCallback callback) :
        User_(user), TimeFrom_(timeFrom), TimeTo_(timeTo), GroupInterval_(groupInterval), Callback_(callback) {}
    void run(AccountingDb *accounting) final { Callback_(accounting->api().queryPPLNSPayoutsAcc(User_, TimeFrom_, TimeTo_, GroupInterval_)); }
  private:
    std::string User_;
    int64_t TimeFrom_;
    int64_t TimeTo_;
    int64_t GroupInterval_;
    QueryPPLNSAccCallback Callback_;
  };

  class TaskPoolLuck : public Task<AccountingDb> {
  public:
    TaskPoolLuck(std::vector<int64_t> &&intervals, PoolLuckCallback callback) : Intervals_(intervals), Callback_(callback) {}
    void run(AccountingDb *accounting) final { Callback_(accounting->api().poolLuck(Intervals_)); }
  private:
    std::vector<int64_t> Intervals_;
    PoolLuckCallback Callback_;
  };

private:
  asyncBase *Base_;
  const PoolBackendConfig &_cfg;
  CCoinInfo CoinInfo_;
  UserManager &UserManager_;
  CNetworkClientDispatcher &ClientDispatcher_;
  CPriceFetcher &PriceFetcher_;
  CFeeEstimationService *FeeEstimationService_ = nullptr;

  std::map<std::string, UserBalanceRecord> _balanceMap;
  std::deque<std::unique_ptr<MiningRound>> _allRounds;
  std::set<MiningRound*> UnpayedRounds_;

  CAccountingState State_;

  class TaskQueryPPSState : public Task<AccountingDb> {

  public:
    TaskQueryPPSState(QueryPPSStateCallback callback) : Callback_(std::move(callback)) {}
    void run(AccountingDb *accounting) final { Callback_(accounting->api().queryPPSState()); }

  private:
    QueryPPSStateCallback Callback_;
  };

  class TaskUpdateBackendSettings : public Task<AccountingDb> {

  public:
    TaskUpdateBackendSettings(
      std::optional<CBackendSettings::PPS> pps,
      std::optional<CBackendSettings::Payouts> payouts,
      DefaultCb callback)
        : PPS_(std::move(pps)),
          Payouts_(std::move(payouts)),
          Callback_(std::move(callback)) {}
    void run(AccountingDb *accounting) final {
      const char *status = accounting->api().updateBackendSettings(PPS_, Payouts_);
      if (strcmp(status, "ok") == 0)
        accounting->notifyInstantPayoutSettingsChanged();
      Callback_(status);
    }

  private:
    std::optional<CBackendSettings::PPS> PPS_;
    std::optional<CBackendSettings::Payouts> Payouts_;
    DefaultCb Callback_;
  };

  kvdb<rocksdbBase> _roundsDb;
  kvdb<rocksdbBase> _balanceDb;
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

  aioUserEvent *ShareLogFlushEvent_ = nullptr;
  bool ShareLogFlushFinished_ = false;

  aioUserEvent *UserStatsFlushEvent_ = nullptr;
  bool UserStatsFlushFinished_ = false;

  aioUserEvent *PPSPayoutEvent_ = nullptr;
  bool PPSPayoutFinished_ = false;

  aioUserEvent *InstantPayoutEvent_ = nullptr;
  bool InstantPayoutFinished_ = false;

  TaskHandlerCoroutine<AccountingDb> TaskHandler_;
  aioUserEvent *FlushTimerEvent_;
  bool ShutdownRequested_ = false;
  bool FlushFinished_ = false;

  CPayoutProcessor PayoutProcessor_;
  CAccountingApi Api_;

  void printRecentStatistic();
  void shareLogFlushHandler();
  void userStatsFlushHandler();
  void ppsPayoutHandler();
  void instantPayoutHandler();
  void ppsPayout();
  void applyPPSCorrection(MiningRound *R);

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
  void cleanupRounds();

  bool applyReward(const std::string &address,
                   const UInt<384> &value,
                   EMiningMode mode,
                   const CRewardParams &rewardParams,
                   kvdb<rocksdbBase>::Batch &balanceBatch,
                   kvdb<rocksdbBase>::Batch &payoutBatch);

  bool hasDeferredReward();
  void calculatePPLNSPayments(MiningRound *R);
  CProcessedWorkSummary processWorkSummaryBatch(const CUserWorkSummaryBatch &batch);
  void onUserWorkSummary(const CUserWorkSummaryBatch &batch);
  void onBlockFound(const CBlockFoundData &block);
  void onUserSettingsUpdate(const UserSettingsRecord &settings);
  void onFeePlanUpdate(const std::string &feePlanId, EMiningMode mode, const std::vector<::UserFeePair> &feeRecord);
  void onFeePlanDelete(const std::string &feePlanId);
  void onUserFeePlanChange(const std::string &login, const std::string &feePlanId);
  bool processRoundConfirmation(MiningRound *R, int64_t confirmations, const std::string &hash);
  void checkBlockConfirmations();
  void checkBlockExtraInfo();

  CAccountingApi &api() { return Api_; }
  CPayoutProcessor &payoutProcessor() { return PayoutProcessor_; }

  std::list<PayoutDbRecord> &getPayoutsQueue() { return State_.PayoutQueue; }
  kvdb<rocksdbBase> &getFoundBlocksDb() { return _foundBlocksDb; }
  kvdb<rocksdbBase> &getPoolBalanceDb() { return _poolBalanceDb; }
  kvdb<rocksdbBase> &getPayoutDb() { return _payoutDb; }
  kvdb<rocksdbBase> &getBalanceDb() { return _balanceDb; }
  kvdb<rocksdbBase> &getPPLNSPayoutsDb() { return PPLNSPayoutsDb; }
  kvdb<rocksdbBase> &getPPSPayoutsDb() { return PPSPayoutsDb; }

  const std::map<std::string, UserBalanceRecord> &getUserBalanceMap() { return _balanceMap; }

  // Asynchronous api
  void manualPayout(const std::string &user, DefaultCb callback) { TaskHandler_.push(new TaskManualPayout(user, callback)); }
  void queryFoundBlocks(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback) {
    TaskHandler_.push(new TaskQueryFoundBlocks(heightFrom, hashFrom, count, callback));
  }
  void queryPPLNSPayouts(const std::string &user, int64_t timeFrom, const std::string &hashFrom, uint32_t count, QueryPPLNSPayoutsCallback callback) {
    TaskHandler_.push(new TaskQueryPPLNSPayouts(user, timeFrom, hashFrom, count, callback));
  }
  void queryPPLNSAcc(const std::string &user, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, QueryPPLNSAccCallback callback) {
    TaskHandler_.push(new TaskQueryPPLNSAcc(user, timeFrom, timeTo, groupByInterval, callback));
  }
  void queryUserBalance(const std::string &user, QueryBalanceCallback callback) { TaskHandler_.push(new TaskQueryBalance(user, callback)); }
  void poolLuck(std::vector<int64_t> &&intervals, PoolLuckCallback callback) { TaskHandler_.push(new TaskPoolLuck(std::move(intervals), callback)); }
  void queryPPSState(QueryPPSStateCallback callback) { TaskHandler_.push(new TaskQueryPPSState(std::move(callback))); }
  void updateBackendSettings(
      std::optional<CBackendSettings::PPS> pps,
      std::optional<CBackendSettings::Payouts> payouts,
      DefaultCb cb) {
    TaskHandler_.push(new TaskUpdateBackendSettings(std::move(pps), std::move(payouts), std::move(cb)));
  }

  CBackendSettings backendSettings() const {
    return State_.BackendSettings.load(std::memory_order_relaxed);
  }
  void setFeeEstimationService(CFeeEstimationService *service) { FeeEstimationService_ = service; }
  void notifyInstantPayoutSettingsChanged() { userEventActivate(InstantPayoutEvent_); }

  // Asynchronous multi calls
  static void queryUserBalanceMulti(AccountingDb **backends, size_t backendsNum, const std::string &user, std::function<void(const UserBalanceInfo*, size_t)> callback) {
    MultiCall<UserBalanceInfo> *context = new MultiCall<UserBalanceInfo>(backendsNum, callback);
    for (size_t i = 0; i < backendsNum; i++)
      backends[i]->queryUserBalance(user, context->generateCallback(i));
  }
};

#endif //__ACCOUNTING_H_
