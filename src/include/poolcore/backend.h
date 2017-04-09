#ifndef __BACKEND_H_
#define __BACKEND_H_

#include "accounting.h"
#include "statistics.h"
#include "asyncio/asyncio.h"
#include "poolcommon/pool_generated.h"
#include "boost/thread.hpp"

class p2pNode;

class PoolBackend {
public:
  struct config {
    bool isMaster;
    unsigned poolFee;
    std::string poolFeeAddr;
    std::string poolAppName;
    std::string walletAppName;
    HostAddress listenAddress;
    std::vector<HostAddress> peers;
    unsigned requiredConfirmations;
    int64_t defaultMinimalPayout;
    int64_t minimalPayout;
    std::string dbPath;
    unsigned keepRoundTime;
    unsigned keepStatsTime;
    unsigned confirmationsCheckInterval;
    unsigned payoutInterval;
    unsigned balanceCheckInterval;
    unsigned statisticCheckInterval;
    
    bool useAsyncPayout;
    std::string poolTAddr;
    std::string poolZAddr;
  };
  
private:
  asyncBase *_base;
  uint64_t _timeout;
  int _pipeFd[2];
  aioObject *_write;
  aioObject *_read;
  boost::thread *_thread;
  
  config _cfg;
  p2pNode *_client;
  p2pNode *_node;
  AccountingDb *_accounting;
  StatisticDb *_statistics;
  
  static void *threadProc(void *arg) { return ((PoolBackend*)arg)->backendMain(); }
  static void *msgHandlerProc(void *arg) { return ((PoolBackend*)arg)->msgHandler(); }
  static void *checkConfirmationsProc(void *arg) { return ((PoolBackend*)arg)->checkConfirmationsHandler(); }  
  static void *payoutProc(void *arg) { return ((PoolBackend*)arg)->payoutHandler(); }    
  static void *checkBalanceProc(void *arg) { return ((PoolBackend*)arg)->checkBalanceHandler(); }    
  static void *updateStatisticProc(void *arg) { return ((PoolBackend*)arg)->updateStatisticHandler(); }   
  
  void *backendMain();
  void *msgHandler();
  void *checkConfirmationsHandler();  
  void *payoutHandler();    
  void *checkBalanceHandler();
  void *updateStatisticHandler();  
  
  void onShare(const Share *share);
  void onStats(const Stats *stats);
  
  
public:
  PoolBackend(config *cfg);
  ~PoolBackend();
  void start();
  void stop();
  bool sendMessage(asyncBase *base, void *msg, uint32_t msgSize);
  
  p2pNode *client() { return _client; }
  AccountingDb *accountingDb() { return _accounting; }
  StatisticDb *statisticDb() { return _statistics; }
};

#endif //__BACKEND_H_
