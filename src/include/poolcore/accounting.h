#ifndef __ACCOUNTING_H_
#define __ACCOUNTING_H_

#include "poolcommon/serialize.h"
#include "backendData.h"
#include "statistics.h"
#include "usermgr.h"
#include "poolcommon/file.h"
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
  std::list<payoutElement> _payoutQueue;  

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
  
  void printRecentStatistic();
  bool parseAccoutingStorageFile(CAccountingFile &file);
  void flushAccountingStorageFile(int64_t timeLabel);

public:
  AccountingDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo, UserManager &userMgr, CNetworkClientDispatcher &clientDispatcher, StatisticDb &statisticDb);

  uint64_t lastAggregatedShareId() { return !AccountingDiskStorage_.empty() ? AccountingDiskStorage_.back().LastShareId : 0; }
  uint64_t lastKnownShareId() { return LastKnownShareId_; }

  void enumerateStatsFiles(std::deque<CAccountingFile> &cache, const std::filesystem::path &directory);
  void start();
  void updatePayoutFile();
  void cleanupRounds();
  
  void requestPayout(const std::string &address, int64_t value, bool force = false);
  void payoutSuccess(const std::string &address, int64_t value, int64_t fee, const std::string &transactionId);

  void addShare(const CShare &share);
  void replayShare(const CShare &share);
  void initializationFinish(int64_t timeLabel);
  void mergeRound(const Round *round);
  void checkBlockConfirmations();
  void makePayout();
  void checkBalance();
  
  std::list<payoutElement> &getPayoutsQueue() { return _payoutQueue; }
  kvdb<rocksdbBase> &getFoundBlocksDb() { return _foundBlocksDb; }
  kvdb<rocksdbBase> &getPoolBalanceDb() { return _poolBalanceDb; }
  kvdb<rocksdbBase> &getPayoutDb() { return _payoutDb; }
  kvdb<rocksdbBase> &getBalanceDb() { return _balanceDb; }

  bool manualPayout(const std::string &user);
  const std::map<std::string, UserBalanceRecord> &getUserBalanceMap() { return _balanceMap; }
};

#endif //__ACCOUNTING_H_
