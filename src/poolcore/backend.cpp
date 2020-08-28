#include "poolcore/backend.h"
#include "poolcore/backendData.h"
#include "poolcore/thread.h"
#include "poolcommon/debug.h"
#include "poolcommon/serialize.h"
#include "asyncio/coroutine.h"
#include "p2putils/xmstream.h"
#include "loguru.hpp"

void ShareLogIo<CShare>::serialize(xmstream &out, const CShare &data)
{
  out.writele<uint64_t>(data.UniqueShareId);
  DbIo<std::string>::serialize(out, data.userId);
  DbIo<std::string>::serialize(out, data.workerId);
  out.write<double>(data.WorkValue);
  DbIo<int64_t>::serialize(out, data.Time);
}

void ShareLogIo<CShare>::unserialize(xmstream &out, CShare &data)
{
  data.UniqueShareId = out.readle<uint64_t>();
  DbIo<std::string>::unserialize(out, data.userId);
  DbIo<std::string>::unserialize(out, data.workerId);
  data.WorkValue = out.read<double>();
  DbIo<int64_t>::unserialize(out, data.Time);
}

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

  _accounting.reset(new AccountingDb(_base, _cfg, CoinInfo_, UserMgr_, ClientDispatcher_));
  _statistics.reset(new StatisticDb(_base, _cfg, CoinInfo_));

  // Enumerate share log files
  std::error_code errc;
  std::filesystem::path directory(cfg.dbPath / "shares.log");
  std::filesystem::create_directories(directory, errc);
  for (std::filesystem::directory_iterator I(directory), IE; I != IE; ++I) {
    std::string fileName = I->path().filename().u8string();
    auto dotDatPos = fileName.find(".dat");
    if (dotDatPos == fileName.npos) {
      LOG_F(ERROR, "Invalid statitic cache file name format: %s", fileName.c_str());
      continue;
    }

    fileName.resize(dotDatPos);
    auto &file = ShareLog_.emplace_back();
    file.Path = *I;
    file.FirstId = xatoi<uint64_t>(fileName.c_str());
    file.LastId = 0;
  }

  std::sort(ShareLog_.begin(), ShareLog_.end(), [](const CShareLogFile &l, const CShareLogFile &r) { return l.FirstId < r.FirstId; });
  if (ShareLog_.empty())
    LOG_F(WARNING, "%s: share log is empty like at first run", CoinInfo_.Name.c_str());

  uint64_t currentTime = time(nullptr);
  for (auto &file: ShareLog_)
    replayShares(file);

  _statistics->initializationFinish(currentTime);

  CurrentShareId_ = std::max({
    _statistics->lastKnownShareId(),
    _accounting->lastKnownShareId()
  }) + 1;

  if (!ShareLog_.empty()) {
    CShareLogFile &lastFile = ShareLog_.back();
    if (lastFile.Fd.open(lastFile.Path)) {
      lastFile.Fd.seekSet(lastFile.Fd.size());
    } else {
      LOG_F(ERROR, "Can't open share log %s", lastFile.Path.u8string().c_str());
      ShareLoggingEnabled_ = false;
    }
  } else {
    startNewShareLogFile();
  }
}

void PoolBackend::startNewShareLogFile()
{
  auto &file = ShareLog_.emplace_back();
  file.Path = _cfg.dbPath / "shares.log" / (std::to_string(CurrentShareId_) + ".dat");
  file.FirstId = CurrentShareId_;
  file.LastId = 0;
  if (!file.Fd.open(file.Path)) {
    LOG_F(ERROR, "PoolBackend: can't write to share log %s", file.Path.u8string().c_str());
    ShareLoggingEnabled_ = false;
  } else {
    LOG_F(INFO, "PoolBackend: started new share log file %s", file.Path.u8string().c_str());
  }
}

void PoolBackend::replayShares(CShareLogFile &file)
{
  if (isDebugBackend())
    LOG_F(1, "%s: Replaying shares from file %s", CoinInfo_.Name.c_str(), file.Path.u8string().c_str());

  FileDescriptor fd;
  if (!fd.open(file.Path.u8string().c_str())) {
    LOG_F(ERROR, "StatisticDb: can't open file %s", file.Path.u8string().c_str());
    return;
  }

  size_t fileSize = fd.size();
  xmstream stream(fileSize);
  size_t bytesRead = fd.read(stream.reserve(fileSize), 0, fileSize);
  fd.close();
  if (bytesRead != fileSize) {
    LOG_F(ERROR, "StatisticDb: can't read file %s", file.Path.u8string().c_str());
    return;
  }

  uint64_t id = 0;
  uint64_t counter = 0;
  uint64_t minShareId = std::numeric_limits<uint64_t>::max();
  uint64_t maxShareId = 0;
  stream.seekSet(0);
  while (stream.remaining()) {
    CShare share;
    ShareLogIo<CShare>::unserialize(stream, share);
    if (stream.eof()) {
      LOG_F(ERROR, "Corrupted file %s", file.Path.u8string().c_str());
      break;
    }

    if (isDebugBackend()) {
      counter++;
      minShareId = std::min(minShareId, share.UniqueShareId);
      maxShareId = std::max(maxShareId, share.UniqueShareId);
    }

    // replay for accounting
    // replay for statistic
    _statistics->replayShare(share);
    // keep last known id
    id = share.UniqueShareId;
  }

  file.LastId = id;

  if (isDebugBackend())
    LOG_F(1, "%s: Replayed %" PRIu64 " shares from %" PRIu64 " to %" PRIu64 "", CoinInfo_.Name.c_str(), counter, minShareId, maxShareId);
}

void PoolBackend::start()
{
  _thread = std::thread([](PoolBackend *backend){ backend->backendMain(); }, this);
}

void PoolBackend::stop()
{
  postQuitOperation(_base);
  _thread.join();
  shareLogFlush();
}


void PoolBackend::backendMain()
{
  InitializeWorkerThread();
  loguru::set_thread_name(CoinInfo_.Name.c_str());

  coroutineCall(coroutineNew([](void *arg) { static_cast<PoolBackend*>(arg)->shareLogFlushHandler(); }, this, 0x100000));
  coroutineCall(coroutineNew([](void *arg) { static_cast<PoolBackend*>(arg)->taskHandler(); }, this, 0x100000));
  coroutineCall(coroutineNew([](void *arg) { static_cast<PoolBackend*>(arg)->checkConfirmationsHandler(); }, this, 0x100000));
  coroutineCall(coroutineNew([](void *arg) { static_cast<PoolBackend*>(arg)->checkBalanceHandler(); }, this, 0x100000));
  coroutineCall(coroutineNew([](void *arg) { static_cast<PoolBackend*>(arg)->payoutHandler(); }, this, 0x100000));

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

void PoolBackend::shareLogFlush()
{
  if (!ShareLoggingEnabled_ || ShareLog_.empty())
    return;

  // Flush memory buffer to disk
  ShareLog_.back().Fd.write(ShareLogInMemory_.data(), ShareLogInMemory_.sizeOf());
  ShareLogInMemory_.reset();

  // Check share log file size limit
  if (ShareLog_.back().Fd.size() >= _cfg.ShareLogFileSizeLimit) {
    ShareLog_.back().Fd.close();
    startNewShareLogFile();

    // Check status of shares in previous log files
    uint64_t aggregatedShareId = std::min({
      _accounting->lastAggregatedShareId(),
      _statistics->lastAggregatedShareId()
    });

    while (!ShareLog_.empty() && ShareLog_.front().LastId < aggregatedShareId) {
      std::filesystem::remove(ShareLog_.front().Path);
      ShareLog_.pop_front();
    }
  }
}

void PoolBackend::shareLogFlushHandler()
{
  aioUserEvent *timerEvent = newUserEvent(_base, 0, nullptr, nullptr);
  for (;;) {
    ioSleep(timerEvent, std::chrono::microseconds(_cfg.ShareLogFlushInterval).count());
    shareLogFlush();
  }
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
  share->UniqueShareId = CurrentShareId_++;
  // Serialize share to stream
  ShareLogIo<CShare>::serialize(ShareLogInMemory_, *share);
  // Processing share
  _accounting->addShare(*share);
  _statistics->addShare(*share);
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
  StatisticDb::CStats aggregate;
  std::vector<StatisticDb::CStats> workers;
  statisticDb()->getUserStats(user, aggregate, workers);
  callback(aggregate, workers);
}

void PoolBackend::queryStatsHistoryImpl(const std::string &user, const std::string &worker, uint64_t timeFrom, uint64_t timeTo, uint64_t groupByInteval, QueryStatsHistoryCallback callback)
{
  std::vector<StatisticDb::CStats> history;
  statisticDb()->getHistory(user, worker, timeFrom, timeTo, groupByInteval, history);
  callback(history);
}
