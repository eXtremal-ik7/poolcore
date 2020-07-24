#ifndef __ACCOUNTING_H_
#define __ACCOUNTING_H_

#include "backendData.h"
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

struct CAccountingShare {
  std::string userId;
  int64_t height;
  int64_t value;
};

struct CAccountingBlock {
  std::string userId;
  int64_t height;
  int64_t value;
  std::string hash;
  int64_t generatedCoins;
};

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
    int64_t shareValue;
    int64_t payoutValue;
    payoutAggregate(const std::string& userIdArg, int64_t shareValueArg) :
      userId(userIdArg), shareValue(shareValueArg), payoutValue(0) {}
  };

private:
  asyncBase *Base_;
  const PoolBackendConfig &_cfg;
  CCoinInfo CoinInfo_;
  UserManager &UserManager_;
  CNetworkClientDispatcher &ClientDispatcher_;
  
  std::map<std::string, UserBalanceRecord> _balanceMap;
  std::map<std::string, int64_t> _currentScores;
  std::deque<miningRound*> _allRounds;
  std::set<miningRound*> _roundsWithPayouts;
  std::list<payoutElement> _payoutQueue;  
  
  FileDescriptor _sharesFd;
  FileDescriptor _payoutsFd;
  kvdb<rocksdbBase> _roundsDb;
  kvdb<rocksdbBase> _balanceDb;
  kvdb<rocksdbBase> _foundBlocksDb;
  kvdb<rocksdbBase> _poolBalanceDb;
  kvdb<rocksdbBase> _payoutDb;
  
  
public:
  AccountingDb(asyncBase *base, const PoolBackendConfig &config, const CCoinInfo &coinInfo, UserManager &userMgr, CNetworkClientDispatcher &clientDispatcher);

  void updatePayoutFile();
  void cleanupRounds();
  
  void requestPayout(const std::string &address, int64_t value, bool force = false);
  void payoutSuccess(const std::string &address, int64_t value, int64_t fee, const std::string &transactionId);
  
  void addShare(const CAccountingShare *share);
  void addBlock(const CAccountingBlock *block, const StatisticDb *statistic);
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
