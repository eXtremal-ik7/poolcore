#ifndef __ACCOUNTING_H_
#define __ACCOUNTING_H_

#include "backendData.h"
#include "poolcommon/pool_generated.h"
#include "poolcommon/file.h"
#include "kvdb.h"
#include "poolcore/rocksdbBase.h"
#include <deque>
#include <list>
#include <map>
#include <string>

static const int64_t COIN = 100000000;

class p2pNode;
class p2pPeer;
class StatisticDb;

typedef bool CheckAddressProcTy(const char*);

class AccountingDb {
public:
  struct config {
    unsigned poolFee;
    std::string poolFeeAddr;
    unsigned requiredConfirmations;
    int64_t defaultMinimalPayout;
    int64_t minimalPayout;
    unsigned keepRoundTime;
    std::filesystem::path dbPath;
    std::string poolZAddr;
    std::string poolTAddr;
    CheckAddressProcTy *checkAddressProc;
    config() : checkAddressProc(0) {}
  };
  
private:
  struct payoutAggregate {
    std::string userId;
    int64_t shareValue;
    int64_t payoutValue;
    payoutAggregate(const std::string& userIdArg, int64_t shareValueArg) :
      userId(userIdArg), shareValue(shareValueArg), payoutValue(0) {}
  };
  
private:
  config _cfg;
  p2pNode *_client;
  
  std::map<std::string, userBalance> _balanceMap; 
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
  AccountingDb(config *cfg, p2pNode *client);
  void updatePayoutFile();
  void cleanupRounds();
  
  void requestPayout(const std::string &address, int64_t value, bool force = false);
  void payoutSuccess(const std::string &address, int64_t value, int64_t fee, const std::string &transactionId);
  
  void addShare(const Share *share, const StatisticDb *statistic);
  void mergeRound(const Round *round);
  void checkBlockConfirmations();
  void makePayout();
  void checkBalance();
  
  std::list<payoutElement> &getPayoutsQueue() { return _payoutQueue; }
  kvdb<rocksdbBase> &getFoundBlocksDb() { return _foundBlocksDb; }
  kvdb<rocksdbBase> &getPoolBalanceDb() { return _poolBalanceDb; }
  kvdb<rocksdbBase> &getPayoutDb() { return _payoutDb; }
  kvdb<rocksdbBase> &getBalanceDb() { return _balanceDb; }
  
  void queryClientBalance(p2pPeer *peer, uint32_t id, const std::string &userId);
  void updateClientInfo(p2pPeer *peer,
                        uint32_t id,
                        const std::string &userId,
                        const std::string &newName,
                        const std::string &newEmail,
                        int64_t newMinimalPayout);
  
  void manualPayout(p2pPeer *peer,
                    uint32_t id,
                    const std::string &userId);  
  
  void resendBrokenTx(p2pPeer *peer,
                      uint32_t id,
                      const std::string &userId);
  
  void moveBalance(p2pPeer *peer, uint32_t id, const std::string &userId, const std::string &to);
};

#endif //__ACCOUNTING_H_
