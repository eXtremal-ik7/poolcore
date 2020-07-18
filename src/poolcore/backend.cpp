#include "poolcore/backend.h"
#include "asyncio/coroutine.h"
#include "p2putils/xmstream.h"
#include "loguru.hpp"

static void checkConsistency(AccountingDb *accounting)
{
  std::map<std::string, int64_t> balancesRequested;
  std::map<std::string, int64_t> queueRequested;
  
  int64_t totalQueued = 0;
  for (auto &p: accounting->getPayoutsQueue()) {
    queueRequested[p.Login] += p.payoutValue;
    totalQueued += p.payoutValue;
  }  
  
  int64_t totalInBalance = 0;
  auto &balanceDb = accounting->getBalanceDb();
  {
    auto *It = balanceDb.iterator();
    It->seekFirst();
    for (; It->valid(); It->next()) {
      UserBalanceRecord balance;
      RawData data = It->value();
      if (!balance.deserializeValue(data.data, data.size))
        break;      
        
      balancesRequested[balance.Login] = balance.Requested;
      totalInBalance += balance.Requested;
    }
  }
  
  LOG_F(INFO, "totalQueued: %li", totalQueued);
  LOG_F(INFO, "totalRequested: %li", totalInBalance);
}


PoolBackend::PoolBackend(PoolBackendConfig &&cfg, const CCoinInfo &info, UserManager &userMgr, CNetworkClientDispatcher &clientDispatcher) :
  _cfg(cfg), CoinInfo_(info), UserMgr_(userMgr), ClientDispatcher_(clientDispatcher)
{
  _base = createAsyncBase(amOSDefault);
  _timeout = 8*1000000;
  TaskQueueEvent_ = newUserEvent(_base, 0, nullptr, nullptr);
}

void PoolBackend::start()
{
  _thread = std::thread([](PoolBackend *backend){ backend->backendMain(); }, this);
}

void PoolBackend::stop()
{
  postQuitOperation(_base);
  _thread.join();
}


void PoolBackend::backendMain()
{
  loguru::set_thread_name(CoinInfo_.Name.c_str());
  _accounting.reset(new AccountingDb(_cfg, CoinInfo_, UserMgr_, ClientDispatcher_));
  _statistics.reset(new StatisticDb(_cfg, CoinInfo_));

  coroutineCall(coroutineNew([](void *arg){ static_cast<PoolBackend*>(arg)->taskHandler(); }, this, 0x100000));
  coroutineCall(coroutineNew(checkConfirmationsProc, this, 0x100000));  
  coroutineCall(coroutineNew(checkBalanceProc, this, 0x100000));      
  coroutineCall(coroutineNew(updateStatisticProc, this, 0x100000));
  coroutineCall(coroutineNew(payoutProc, this, 0x100000)); 
  
  LOG_F(INFO, "<info>: Pool backend for '%s' started, mode is %s", CoinInfo_.Name.c_str(), _cfg.isMaster ? "MASTER" : "SLAVE");
  if (!_cfg.PoolFee.empty()) {
    for (const auto &poolFeeEntry: _cfg.PoolFee)
      LOG_F(INFO, "  Pool fee of %.2f to %s", poolFeeEntry.Percentage, poolFeeEntry.Address.c_str());
  } else {
    LOG_F(INFO, "  Pool fee disabled");
  }
  
  checkConsistency(_accounting.get());
  asyncLoop(_base);
}

void PoolBackend::taskHandler()
{
  Task *task;
  for (;;) {
    while (TaskQueue_.try_pop(task)) {
      std::unique_ptr<Task> taskHolder(task);
      task->run(this);
    }

    ioWaitUserEvent(TaskQueueEvent_);
  }
}

void *PoolBackend::checkConfirmationsHandler()
{
  aioUserEvent *timerEvent = newUserEvent(_base, 0, nullptr, nullptr);
  while (true) {
    ioSleep(timerEvent, _cfg.ConfirmationsCheckInterval);
    _accounting->cleanupRounds();
    _accounting->checkBlockConfirmations();
  }
}


void *PoolBackend::payoutHandler()
{
  aioUserEvent *timerEvent = newUserEvent(_base, 0, nullptr, nullptr);
  while (true) {
    ioSleep(timerEvent, _cfg.PayoutInterval);
    _accounting->makePayout();
  }
}

// Only for master
void *PoolBackend::checkBalanceHandler()
{
  aioUserEvent *timerEvent = newUserEvent(_base, 0, nullptr, nullptr);
  while (true) {
    _accounting->checkBalance();
    ioSleep(timerEvent, _cfg.BalanceCheckInterval);
  }
}

void *PoolBackend::updateStatisticHandler()
{
  aioUserEvent *timerEvent = newUserEvent(_base, 0, nullptr, nullptr);
  while (true) {
    ioSleep(timerEvent, _cfg.StatisticCheckInterval);
    _statistics->update();
  }
}

void PoolBackend::onShare(const Share *share)
{
  _accounting->addShare(share, _statistics.get());
}

void PoolBackend::onStats(const Stats *stats)
{
  _statistics->addStats(stats);
}
