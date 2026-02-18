#ifndef __ACCOUNTING_H_
#define __ACCOUNTING_H_

#include "backendData.h"
#include "shareLog.h"
#include "statsData.h"
#include "usermgr.h"
#include "poolcommon/multiCall.h"
#include "poolcommon/taskHandler.h"
#include "poolcore/clientDispatcher.h"
#include <atomic>

class CPriceFetcher;

inline std::string ppsMetaUserId() { return std::string("pps\0meta", 8); }

enum class ESaturationFunction : uint32_t {
  None = 0,
  Tangent,
  Clamp,
  Cubic,
  Softsign,
  Norm,
  Atan,
  Exp,
};

struct CPPSConfig {

public:
  static bool parseSaturationFunction(const std::string &name, ESaturationFunction *out);
  static const char *saturationFunctionName(ESaturationFunction value);
  static const std::vector<const char*> &saturationFunctionNames();
  double saturateCoeff(double balanceInBlocks) const;

  double PoolFee = 4.0;
  ESaturationFunction SaturationFunction = ESaturationFunction::None;
  double SaturationB0 = 0.0;
  double SaturationANegative = 0.0;
  double SaturationAPositive = 0.0;
};

struct CPPSBalanceSnapshot {

public:
  UInt<384> Balance;
  double TotalBlocksFound = 0.0;
  Timestamp Time;
};

struct CPPSState {

public:
  // Pool-side PPS balance: increases when block found (PPS deduction from PPLNS),
  // decreases when PPS rewards are accrued to users.
  // Can go negative — that's the pool's risk.
  UInt<384> Balance;
  // Base reward of the last known block (subsidy without tx fees, fixed-point 128.256)
  UInt<384> LastBaseBlockReward;
  // Fractional count of blocks found (only PPS portion of each block)
  double TotalBlocksFound = 0.0;

  CPPSBalanceSnapshot Min;
  CPPSBalanceSnapshot Max;

  // Last applied saturation coefficient (1.0 = no correction)
  double LastSaturateCoeff = 1.0;
  // Last average transaction fee per block (fixed-point 128.256)
  UInt<384> LastAverageTxFee;

  // Timestamp of this snapshot (used as kvdb key for history)
  Timestamp Time;

  static double balanceInBlocks(const UInt<384> &balance, const UInt<384> &baseBlockReward);
  static double sqLambda(
    const UInt<384> &balance,
    const UInt<384> &baseBlockReward,
    double totalBlocksFound);
  void updateMinMax(Timestamp now);

  std::string getPartitionId() const { return partByTime(Time.toUnixTime()); }
  void serializeKey(xmstream &stream) const;
  void serializeValue(xmstream &stream) const;
  bool deserializeValue(const void *data, size_t size);
};

class AccountingDb {
public:
  struct UserBalanceInfo {
    UserBalanceRecord Data;
    UInt<384> Queued;
  };

  struct CPPLNSPayoutAcc {
    int64_t IntervalEnd = 0;
    UInt<384> TotalCoin = UInt<384>::zero();
    UInt<384> TotalBTC = UInt<384>::zero();
    UInt<384> TotalUSD = UInt<384>::zero();
    UInt<256> TotalIncomingWork = UInt<256>::zero();
    uint64_t AvgHashRate = 0;
    uint32_t PrimePOWTarget = -1U;

    void merge(const CPPLNSPayout &record, unsigned fractionalPartSize);
    void mergeScaled(const CPPLNSPayout &record, double coeff, unsigned fractionalPartSize);
  };


private:
  using DefaultCb = std::function<void(const char*)>;
  using ManualPayoutCallback = std::function<void(bool)>;
  using QueryFoundBlocksCallback = std::function<void(const std::vector<FoundBlockRecord>&, const std::vector<CNetworkClient::GetBlockConfirmationsQuery>&)>;
  using QueryBalanceCallback = std::function<void(const UserBalanceInfo&)>;
  using QueryPPLNSPayoutsCallback = std::function<void(const std::vector<CPPLNSPayout>&)>;
  using QueryPPLNSAccCallback = std::function<void(const std::vector<CPPLNSPayoutAcc>&)>;
  using PoolLuckCallback = std::function<void(const std::vector<double>&)>;
  using QueryPPSConfigCallback = std::function<void(bool enabled, const CPPSConfig&)>;

  struct UserFeePair {
    std::string UserId;
    double FeeCoeff;
    UserFeePair(const std::string &userId, double fee) : UserId(userId), FeeCoeff(fee) {}
  };

  class TaskManualPayout : public Task<AccountingDb> {
  public:
    TaskManualPayout(const std::string &user, DefaultCb callback) : User_(user), Callback_(callback) {}
    void run(AccountingDb *accounting) final { accounting->manualPayoutImpl(User_, Callback_); }
  private:
    std::string User_;
    DefaultCb Callback_;
  };

  class TaskQueryFoundBlocks : public Task<AccountingDb> {
  public:
    TaskQueryFoundBlocks(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback) : HeightFrom_(heightFrom), HashFrom_(hashFrom), Count_(count), Callback_(callback) {}
    void run(AccountingDb *accounting) final { accounting->queryFoundBlocksImpl(HeightFrom_, HashFrom_, Count_, Callback_); }
  private:
    int64_t HeightFrom_;
    std::string HashFrom_;
    uint32_t Count_;
    QueryFoundBlocksCallback Callback_;
  };

  class TaskQueryBalance : public Task<AccountingDb> {
  public:
    TaskQueryBalance(const std::string &user, QueryBalanceCallback callback) : User_(user), Callback_(callback) {}
    void run(AccountingDb *accounting) final { accounting->queryBalanceImpl(User_, Callback_); }
  private:
    std::string User_;
    QueryBalanceCallback Callback_;
  };

  class TaskQueryPPLNSPayouts : public Task<AccountingDb> {
  public:
    TaskQueryPPLNSPayouts(const std::string &user, int64_t timeFrom, const std::string &hashFrom, uint32_t count, QueryPPLNSPayoutsCallback callback) :
      User_(user), TimeFrom_(timeFrom), HashFrom_(hashFrom), Count_(count), Callback_(callback) {}
    void run(AccountingDb *accounting) final { accounting->queryPPLNSPayoutsImpl(User_, TimeFrom_, HashFrom_, Count_, Callback_); }
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
    void run(AccountingDb *accounting) final { accounting->queryPPLNSPayoutsAccImpl(User_, TimeFrom_, TimeTo_, GroupInterval_, Callback_); }
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
    void run(AccountingDb *accounting) final { accounting->poolLuckImpl(Intervals_, Callback_); }
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
  // Persistent state stored in accounting.state RocksDB
  struct CPersistentState {

  public:
    // Accumulated share work per user for the current block search session; cleared on block found
    std::map<std::string, UInt<256>> CurrentScores;
    // Accumulated PPS value per user (fixed-point 128.256 format); cleared on PPS payout
    std::unordered_map<std::string, UInt<384>> PPSPendingBalance;
    std::vector<CStatsExportData> RecentStats;
    uint64_t SavedShareId = 0;
    Timestamp CurrentRoundStartTime;
    std::list<PayoutDbRecord> PayoutQueue;
    std::unordered_set<std::string> KnownTransactions;

    std::atomic<bool> PPSModeEnabled{false};
    CPPSConfig PPSConfig;
    CPPSState PPSState;

    rocksdbBase Db;

    CPersistentState(const std::filesystem::path &dbPath);
    bool load();
    uint64_t lastAcceptedMsgId() const { return LastAcceptedMsgId_; }

    void addScores(uint64_t msgId, const CUserWorkSummaryBatch &batch) {
      if (msgId <= LastAcceptedMsgId_)
        return;
      LastAcceptedMsgId_ = msgId;
      for (const auto &entry : batch.Entries)
        CurrentScores[entry.UserId] += entry.AcceptedWork;
    }

    void setLastAcceptedMsgId(uint64_t msgId) { LastAcceptedMsgId_ = msgId; }

    void flushState();
    void flushPPSConfig();
    void flushPayoutQueue();

  private:
    uint64_t LastAcceptedMsgId_ = 0;
  };

  CPersistentState State_;

  class TaskQueryPPSConfig : public Task<AccountingDb> {

  public:
    TaskQueryPPSConfig(QueryPPSConfigCallback callback) : Callback_(std::move(callback)) {}
    void run(AccountingDb *accounting) final { accounting->queryPPSConfigImpl(Callback_); }

  private:
    QueryPPSConfigCallback Callback_;
  };

  class TaskUpdatePPSConfig : public Task<AccountingDb> {

  public:
    TaskUpdatePPSConfig(bool enabled, CPPSConfig cfg, DefaultCb callback) :
      PPSModeEnabled_(enabled), Config_(std::move(cfg)), Callback_(std::move(callback)) {}
    void run(AccountingDb *accounting) final {
      accounting->updatePPSConfigImpl(PPSModeEnabled_, Config_, Callback_);
    }

  private:
    bool PPSModeEnabled_;
    CPPSConfig Config_;
    DefaultCb Callback_;
  };

  kvdb<rocksdbBase> _roundsDb;
  kvdb<rocksdbBase> _balanceDb;
  kvdb<rocksdbBase> _foundBlocksDb;
  kvdb<rocksdbBase> _poolBalanceDb;
  kvdb<rocksdbBase> _payoutDb;
  kvdb<rocksdbBase> PPLNSPayoutsDb;
  kvdb<rocksdbBase> PPSHistoryDb_;
  ShareLog<CUserWorkSummaryBatch> ShareLog_;

  std::unordered_map<std::string, UserSettingsRecord> UserSettings_;

  CStatsSeriesMap UserStatsAcc_;
  aioUserEvent *UserStatsFlushEvent_;
  bool UserStatsFlushFinished_ = false;

  aioUserEvent *ShareLogFlushEvent_ = nullptr;
  bool ShareLogFlushFinished_ = false;

  aioUserEvent *PPSPayoutEvent_ = nullptr;
  bool PPSPayoutFinished_ = false;

  TaskHandlerCoroutine<AccountingDb> TaskHandler_;
  aioUserEvent *FlushTimerEvent_;
  bool ShutdownRequested_ = false;
  bool FlushFinished_ = false;

  void printRecentStatistic();
  void flushUserStats(Timestamp currentTime);
  void shareLogFlushHandler();
  void ppsPayoutHandler();
  void ppsPayout();

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
  
  bool requestPayout(const std::string &address, const UInt<384> &value, bool force = false);

  bool hasUnknownReward();
  void calculatePayments(MiningRound *R, const UInt<384> &generatedCoins);
  void onUserWorkSummary(const CUserWorkSummaryBatch &batch);
  void onBlockFound(const CBlockFoundData &block);
  void onUserSettingsUpdate(const UserSettingsRecord &settings);
  void processRoundConfirmation(MiningRound *R, int64_t confirmations, const std::string &hash, bool *roundUpdated);
  void checkBlockConfirmations();
  void checkBlockExtraInfo();
  void buildTransaction(PayoutDbRecord &payout, unsigned index, std::string &recipient, bool *needSkipPayout);
  bool sendTransaction(PayoutDbRecord &payout);
  bool checkTxConfirmations(PayoutDbRecord &payout);
  void makePayout();
  void checkBalance();
  
  std::list<PayoutDbRecord> &getPayoutsQueue() { return State_.PayoutQueue; }
  kvdb<rocksdbBase> &getFoundBlocksDb() { return _foundBlocksDb; }
  kvdb<rocksdbBase> &getPoolBalanceDb() { return _poolBalanceDb; }
  kvdb<rocksdbBase> &getPayoutDb() { return _payoutDb; }
  kvdb<rocksdbBase> &getBalanceDb() { return _balanceDb; }
  kvdb<rocksdbBase> &getPPLNSPayoutsDb() { return PPLNSPayoutsDb; }

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
  void queryPPSConfig(QueryPPSConfigCallback callback) { TaskHandler_.push(new TaskQueryPPSConfig(std::move(callback))); }
  void updatePPSConfig(bool enabled, CPPSConfig cfg, DefaultCb cb) {
    TaskHandler_.push(new TaskUpdatePPSConfig(enabled, std::move(cfg), cb));
  }

  bool isPPSEnabled() const { return State_.PPSModeEnabled.load(std::memory_order_relaxed); }
  void setFeeEstimationService(CFeeEstimationService *service) { FeeEstimationService_ = service; }

  // Asynchronous multi calls
  static void queryUserBalanceMulti(AccountingDb **backends, size_t backendsNum, const std::string &user, std::function<void(const UserBalanceInfo*, size_t)> callback) {
    MultiCall<UserBalanceInfo> *context = new MultiCall<UserBalanceInfo>(backendsNum, callback);
    for (size_t i = 0; i < backendsNum; i++)
      backends[i]->queryUserBalance(user, context->generateCallback(i));
  }

private:
  void manualPayoutImpl(const std::string &user, DefaultCb callback);
  void queryBalanceImpl(const std::string &user, QueryBalanceCallback callback);
  void queryFoundBlocksImpl(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback);
  void queryPPLNSPayoutsImpl(const std::string &login, int64_t timeFrom, const std::string &hashFrom, uint32_t count, QueryPPLNSPayoutsCallback callback);
  void queryPPLNSPayoutsAccImpl(const std::string &login, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, QueryPPLNSAccCallback callback);
  void poolLuckImpl(const std::vector<int64_t> &intervals, PoolLuckCallback callback);
  void queryPPSConfigImpl(QueryPPSConfigCallback callback);
  void updatePPSConfigImpl(bool enabled, const CPPSConfig &cfg, DefaultCb callback);
};

#endif //__ACCOUNTING_H_
