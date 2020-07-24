#include "poolcore/backend.h"
#include "poolcore/backendData.h"
#include "poolcore/thread.h"
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
  clientDispatcher.setBackend(this);
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
  InitializeWorkerThread();
  loguru::set_thread_name(CoinInfo_.Name.c_str());
  _accounting.reset(new AccountingDb(_base, _cfg, CoinInfo_, UserMgr_, ClientDispatcher_));
  _statistics.reset(new StatisticDb(_cfg, CoinInfo_));

  coroutineCall(coroutineNew([](void *arg){ static_cast<PoolBackend*>(arg)->taskHandler(); }, this, 0x100000));
  coroutineCall(coroutineNew(checkConfirmationsProc, this, 0x100000));
  coroutineCall(coroutineNew(checkBalanceProc, this, 0x100000));
  coroutineCall(coroutineNew(updateStatisticProc, this, 0x100000));
  coroutineCall(coroutineNew(payoutProc, this, 0x100000));

  LOG_F(INFO, "<info>: Pool backend for '%s' started, mode is %s, tid=%u", CoinInfo_.Name.c_str(), _cfg.isMaster ? "MASTER" : "SLAVE", GetGlobalThreadId());
  if (!_cfg.PoolFee.empty()) {
    for (const auto &poolFeeEntry: _cfg.PoolFee)
      LOG_F(INFO, "  Pool fee of %.2f to %s", poolFeeEntry.Percentage, poolFeeEntry.User.c_str());
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

void PoolBackend::onShare(const CAccountingShare *share)
{
  _accounting->addShare(share);
}

void PoolBackend::onBlock(const CAccountingBlock *block)
{
  _accounting->addBlock(block, _statistics.get());
}

void PoolBackend::onStats(const CUserStats *stats)
{
  _statistics->addStats(stats);
}

void PoolBackend::manualPayoutImpl(const std::string &user, ManualPayoutCallback callback)
{
  callback(_accounting->manualPayout(user));
}

void PoolBackend::queryFoundBlocksImpl(int64_t heightFrom, const std::string &hashFrom, uint32_t count, QueryFoundBlocksCallback callback)
{
  auto &db = accountingDb()->getFoundBlocksDb();
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());
  if (heightFrom != -1) {
    FoundBlockRecord blk;
    blk.Height = heightFrom;
    blk.Hash = hashFrom;
    It->seek(blk);
    It->prev();
  } else {
    It->seekLast();
  }

  std::vector<CNetworkClient::GetBlockConfirmationsQuery> confirmationsQuery;
  std::vector<FoundBlockRecord> foundBlocks;
  for (unsigned i = 0; i < count && It->valid(); i++) {
    FoundBlockRecord dbBlock;
    RawData data = It->value();
    if (!dbBlock.deserializeValue(data.data, data.size))
      break;
    foundBlocks.push_back(dbBlock);
    confirmationsQuery.emplace_back(dbBlock.Hash, dbBlock.Height);
    It->prev();
  }

  // query confirmations
  if (count)
    ClientDispatcher_.ioGetBlockConfirmations(_base, confirmationsQuery);

  callback(foundBlocks, confirmationsQuery);
}

void PoolBackend::queryBalanceImpl(const std::string &user, QueryBalanceCallback callback)
{
  auto &balanceMap = accountingDb()->getUserBalanceMap();
  auto It = balanceMap.find(user);
  if (It != balanceMap.end()) {
    callback(It->second);
  } else {
    UserBalanceRecord record;
    record.Login = user;
    record.Balance = 0;
    record.Requested = 0;
    record.Paid = 0;
    callback(record);
  }
}

void PoolBackend::queryPayouts(const std::string &user, uint64_t timeFrom, unsigned count, std::vector<PayoutDbRecord> &payouts)
{
  auto &db = accountingDb()->getPayoutDb();
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());

  {
    PayoutDbRecord pr;
    pr.userId = user;
    pr.time = timeFrom == 0 ? std::numeric_limits<uint64_t>::max() : timeFrom;
    It->seek(pr);
    It->prev();
  }

  for (unsigned i = 0; i < count; i++) {
    if (!It->valid())
      break;

    payouts.push_back(PayoutDbRecord());
    RawData data = It->value();
    if (!payouts.back().deserializeValue(data.data, data.size) || payouts.back().userId != user)
      break;
    It->prev();
  }
}

void PoolBackend::queryPoolStatsImpl(QueryPoolStatsCallback callback)
{
  callback(statisticDb()->getPoolStats());
}

void PoolBackend::queryUserStatsImpl(const std::string &user, QueryUserStatsCallback callback)
{
  SiteStatsRecord aggregate;
  std::vector<ClientStatsRecord> workers;
  statisticDb()->getUserStats(user, aggregate, workers);
  callback(aggregate, workers);
}
