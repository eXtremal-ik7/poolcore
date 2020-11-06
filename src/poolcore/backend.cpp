#include "poolcore/backend.h"
#include "poolcore/backendData.h"
#include "poolcore/thread.h"
#include "poolcommon/debug.h"
#include "poolcommon/serialize.h"
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


PoolBackend::PoolBackend(const PoolBackendConfig &cfg, const CCoinInfo &info, UserManager &userMgr, CNetworkClientDispatcher &clientDispatcher, CPriceFetcher &priceFetcher) :
  _cfg(cfg), CoinInfo_(info), UserMgr_(userMgr), ClientDispatcher_(clientDispatcher), PriceFetcher_(priceFetcher)
{
  clientDispatcher.setBackend(this);
  _base = createAsyncBase(amOSDefault);
  _timeout = 8*1000000;
  TaskQueueEvent_ = newUserEvent(_base, 0, nullptr, nullptr);

  _statistics.reset(new StatisticDb(_base, _cfg, CoinInfo_));
  _accounting.reset(new AccountingDb(_base, _cfg, CoinInfo_, UserMgr_, ClientDispatcher_, *_statistics.get()));

  ShareLogConfig shareLogConfig(_accounting.get(), _statistics.get());
  ShareLog_.init(cfg.dbPath / "shares.log", info.Name, _base, _cfg.ShareLogFlushInterval, _cfg.ShareLogFileSizeLimit, shareLogConfig);

  ProfitSwitchCoeff_ = CoinInfo_.ProfitSwitchDefaultCoeff;
}

void PoolBackend::start()
{
  _thread = std::thread([](PoolBackend *backend){ backend->backendMain(); }, this);
}

void PoolBackend::stop()
{
  postQuitOperation(_base);
  _thread.join();
  ShareLog_.flush();
}

void PoolBackend::backendMain()
{
  InitializeWorkerThread();
  loguru::set_thread_name(CoinInfo_.Name.c_str());
  ShareLog_.start();

  coroutineCall(coroutineNew([](void *arg) { static_cast<PoolBackend*>(arg)->taskHandler(); }, this, 0x100000));
  coroutineCall(coroutineNew([](void *arg) { static_cast<PoolBackend*>(arg)->checkConfirmationsHandler(); }, this, 0x100000));
  coroutineCall(coroutineNew([](void *arg) { static_cast<PoolBackend*>(arg)->checkBalanceHandler(); }, this, 0x100000));
  coroutineCall(coroutineNew([](void *arg) { static_cast<PoolBackend*>(arg)->payoutHandler(); }, this, 0x100000));

  _accounting->start();
  _statistics->start();

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

void PoolBackend::onShare(CShare *share)
{
  ShareLog_.addShare(*share);
  _statistics->addShare(*share);
  _accounting->addShare(*share);
}

void PoolBackend::queryPayouts(const std::string &user, uint64_t timeFrom, unsigned count, std::vector<PayoutDbRecord> &payouts)
{
  auto &db = accountingDb()->getPayoutDb();
  std::unique_ptr<rocksdbBase::IteratorType> It(db.iterator());


  xmstream resumeKey;
  {
    PayoutDbRecord record;
    record.userId = user;
    record.time = std::numeric_limits<int64_t>::max();
    record.serializeKey(resumeKey);
  }

  {
    PayoutDbRecord record;
    record.userId = user;
    record.time = timeFrom == 0 ? std::numeric_limits<int64_t>::max() : timeFrom;
    It->seekForPrev(record);
  }

  auto endPredicate = [&user](const void *key, size_t size) -> bool {
    PayoutDbRecord record;
    if (!record.deserializeValue(key, size)) {
      LOG_F(ERROR, "Statistic database corrupt!");
      return true;
    }

    return record.userId != user;
  };

  for (unsigned i = 0; i < count; i++) {
    if (!It->valid())
      break;

    RawData data = It->value();
    PayoutDbRecord &payout = payouts.emplace_back();
    if (!payout.deserializeValue(data.data, data.size) || payout.userId != user)
      break;

    It->prev(endPredicate, resumeKey.data(), resumeKey.sizeOf());
  }
}
