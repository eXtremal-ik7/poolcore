#ifndef __ACCOUNTING_H_
#define __ACCOUNTING_H_

#include "poolcommon/serialize.h"
#include "backendData.h"
#include "statistics.h"
#include "usermgr.h"
#include "poolcommon/file.h"
#include "poolcommon/multiCall.h"
#include "poolcommon/taskHandler.h"
#include "poolcore/clientDispatcher.h"
#include "kvdb.h"
#include "poolcore/rocksdbBase.h"
#include <deque>
#include <list>
#include <map>
#include <set>
#include <string>

class p2pNode;
class p2pPeer;
class StatisticDb;

struct RoundElement {
  std::string userId;
  int64_t shareValue;
};

struct Round {
  int64_t height;
  std::string hash;
  uint64_t time;
  int64_t availableCoins;
  std::vector<RoundElement> elements;
};

class AccountingDb {
private:
  using ManualPayoutCallback = std::function<void(bool)>;
  using QueryFoundBlocksCallback = std::function<void(const std::vector<FoundBlockRecord>&, const std::vector<CNetworkClient::GetBlockConfirmationsQuery>&)>;
  using QueryBalanceCallback = std::function<void(const UserBalanceRecord&)>;

private:
  struct UserFeePair {
    std::string UserId;
    double FeeCoeff;
    UserFeePair(const std::string &userId, double fee) : UserId(userId), FeeCoeff(fee) {}
  };

  struct payoutAggregate {
    std::string userId;
    double shareValue;
    int64_t payoutValue;
  };

  struct CAccountingFile {
    int64_t TimeLabel = 0;
    uint64_t LastShareId = 0;
    std::filesystem::path Path;
  };

  struct CFlushInfo {
    uint64_t ShareId;
    int64_t Time;
  };

  class TaskShare : public Task<AccountingDb> {
  public:
    TaskShare(CShare *share) : Share_(share) {}
    void run(AccountingDb *accounting) final { accounting->addShare(*Share_); }
  private:
    std::unique_ptr<CShare> Share_;
  };

  class TaskManualPayout : public Task<AccountingDb> {
  public:
    TaskManualPayout(const std::string &user, ManualPayoutCallback callback) : User_(user), Callback_(callback) {}
    void run(AccountingDb *accounting) final { accounting->manualPayoutImpl(User_, Callback_); }
  private:
    std::string User_;
    ManualPayoutCallback Callback_;
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

private:
  asyncBase *Base_;
  const PoolBackendConfig &_cfg;
  CCoinInfo CoinInfo_;
  UserManager &UserManager_;
  CNetworkClientDispatcher &ClientDispatcher_;
  StatisticDb &StatisticDb_;
  
  std::map<std::string, UserBalanceRecord> _balanceMap;
  std::deque<miningRound*> _allRounds;
  std::set<miningRound*> _roundsWithPayouts;
  std::list<PayoutDbRecord> _payoutQueue;

  int64_t LastBlockTime_ = 0;
  std::deque<CAccountingFile> AccountingDiskStorage_;
  std::map<std::string, double> CurrentScores_;
  std::vector<StatisticDb::CStatsExportData> RecentStats_;
  CFlushInfo FlushInfo_;

  // Debugging only
  struct {
    uint64_t MinShareId = std::numeric_limits<uint64_t>::max();
    uint64_t MaxShareId = 0;
    uint64_t Count = 0;
  } Dbg_;

  FileDescriptor _payoutsFd;
  kvdb<rocksdbBase> _roundsDb;
  kvdb<rocksdbBase> _balanceDb;
  kvdb<rocksdbBase> _foundBlocksDb;
  kvdb<rocksdbBase> _poolBalanceDb;
  kvdb<rocksdbBase> _payoutDb;
  
  uint64_t LastKnownShareId_ = 0;
  
  TaskHandlerCoroutine<AccountingDb> TaskHandler_;
  aioUserEvent *FlushTimerEvent_;
  bool ShutdownRequested_ = false;
  bool FlushFinished_ = false;

  void printRecentStatistic();
  bool parseAccoutingStorageFile(CAccountingFile &file);
  void flushAccountingStorageFile(int64_t timeLabel);

public:
  AccountingDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo, UserManager &userMgr, CNetworkClientDispatcher &clientDispatcher, StatisticDb &statisticDb);
  void taskHandler();

  uint64_t lastAggregatedShareId() { return !AccountingDiskStorage_.empty() ? AccountingDiskStorage_.back().LastShareId : 0; }
  uint64_t lastKnownShareId() { return LastKnownShareId_; }

  void enumerateStatsFiles(std::deque<CAccountingFile> &cache, const std::filesystem::path &directory);
  void start();
  void stop();
  void updatePayoutFile();
  void cleanupRounds();
  
  bool requestPayout(const std::string &address, int64_t value, bool force = false);

  void processPersonalFee(UserManager::PersonalFeeTree &personalFeeConfig, UserShareValue &shareValue, std::map<std::string, double> &personalFeeMap);
  void processPersonalFeeImpl(UserManager::PersonalFeeNode *node, std::map<std::string, double> &personalFeeMap);

  void addShare(const CShare &share);
  void replayShare(const CShare &share);
  void initializationFinish(int64_t timeLabel);
  void mergeRound(const Round *round);
  void checkBlockConfirmations();
  void buildTransaction(PayoutDbRecord &payout, unsigned index, std::string &recipient, bool *needSkipPayout);
  bool sendTransaction(PayoutDbRecord &payout);
  bool checkTxConfirmations(PayoutDbRecord &payout);
  void makePayout();
  void checkBalance();
  
  std::list<PayoutDbRecord> &getPayoutsQueue() { return _payoutQueue; }
  kvdb<rocksdbBase> &getFoundBlocksDb() { return _foundBlocksDb; }
  kvdb<rocksdbBase> &getPoolBalanceDb() { return _poolBalanceDb; }
  kvdb<rocksdbBase> &getPayoutDb() { return _payoutDb; }
  kvdb<rocksdbBase> &getBalanceDb() { return _balanceDb; }

  const std::map<std::string, UserBalanceRecord> &getUserBalanceMap() { return _balanceMap; }

  // Asynchronous api
  void manualPayout(const std::string &user, ManualPayoutCallback callback) { TaskHandler_.push(new TaskManualPayout(user, callback)); }
  void queryFoundBlocks(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback) { TaskHandler_.push(new TaskQueryFoundBlocks(heightFrom, hashFrom, count, callback)); }
  void queryUserBalance(const std::string &user, QueryBalanceCallback callback) { TaskHandler_.push(new TaskQueryBalance(user, callback)); }

  // Asynchronous multi calls
  static void queryUserBalanceMulti(AccountingDb **backends, size_t backendsNum, const std::string &user, std::function<void(const UserBalanceRecord*, size_t)> callback) {
    MultiCall<UserBalanceRecord> *context = new MultiCall<UserBalanceRecord>(backendsNum, callback);
    for (size_t i = 0; i < backendsNum; i++)
      backends[i]->queryUserBalance(user, context->generateCallback(i));
  }

private:
  void manualPayoutImpl(const std::string &user, ManualPayoutCallback callback);
  void queryBalanceImpl(const std::string &user, QueryBalanceCallback callback);
  void queryFoundBlocksImpl(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback);
};

#endif //__ACCOUNTING_H_
