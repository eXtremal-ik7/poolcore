#ifndef __BACKEND_H_
#define __BACKEND_H_

#include "accounting.h"
#include "statistics.h"
#include "usermgr.h"
#include "asyncio/asyncio.h"
#include "asyncio/device.h"
#include "poolcommon/pool_generated.h"
#include <thread>

class p2pNode;

class PoolBackend {
private:
  asyncBase *_base;
  uint64_t _timeout;
  pipeTy _pipeFd;
  aioObject *_write;
  aioObject *_read;
  std::thread _thread;
  
  PoolBackendConfig _cfg;
  UserManager &UserMgr_;
  std::unique_ptr<AccountingDb> _accounting;
  std::unique_ptr<StatisticDb> _statistics;

  static void msgHandlerProc(void *arg) { ((PoolBackend*)arg)->msgHandler(); }
  static void checkConfirmationsProc(void *arg) { ((PoolBackend*)arg)->checkConfirmationsHandler(); }
  static void payoutProc(void *arg) { ((PoolBackend*)arg)->payoutHandler(); }
  static void checkBalanceProc(void *arg) { ((PoolBackend*)arg)->checkBalanceHandler(); }
  static void updateStatisticProc(void *arg) { ((PoolBackend*)arg)->updateStatisticHandler(); }
  
  void backendMain();
  void *msgHandler();
  void *checkConfirmationsHandler();  
  void *payoutHandler();    
  void *checkBalanceHandler();
  void *updateStatisticHandler();  
  
  void onShare(const Share *share);
  void onStats(const Stats *stats);
  
  
public:
  PoolBackend(const PoolBackend&) = delete;
  PoolBackend(PoolBackend&&) = default;
  PoolBackend(PoolBackendConfig &&cfg, UserManager &userMgr);
  const PoolBackendConfig &getConfig() { return _cfg; }

  void start();
  void stop();
  bool sendMessage(asyncBase *base, void *msg, uint32_t msgSize);

  AccountingDb *accountingDb() { return _accounting.get(); }
  StatisticDb *statisticDb() { return _statistics.get(); }
};

#endif //__BACKEND_H_
