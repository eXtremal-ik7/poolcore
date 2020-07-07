#include "api.h"
#include "poolcore/backend.h"
#include "asyncio/coroutine.h"
#include "p2p/p2p.h"
#include "p2putils/xmstream.h"
#include "loguru.hpp"

static void checkConsistency(AccountingDb *accounting)
{
  std::map<std::string, int64_t> balancesRequested;
  std::map<std::string, int64_t> queueRequested;
  
  int64_t totalQueued = 0;
  for (auto &p: accounting->getPayoutsQueue()) {
    queueRequested[p.userId] += p.payoutValue;
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


PoolBackend::PoolBackend(PoolBackendConfig &&cfg) : _cfg(cfg)
{
  _base = createAsyncBase(amOSDefault);
  _timeout = 8*1000000;
  pipeCreate(&_pipeFd, 1);
  _read = newDeviceIo(_base, _pipeFd.read);
  _write = newDeviceIo(_base, _pipeFd.write);
}

bool PoolBackend::sendMessage(asyncBase *base, void *msg, uint32_t msgSize)
{
  xmstream stream;
  stream.write<uint32_t>(msgSize);
  stream.write(msg, msgSize);
  return ioWrite(_write, stream.data(), stream.sizeOf(), afWaitAll, _timeout) == stream.sizeOf();
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
  loguru::set_thread_name(_cfg.CoinName.c_str());
  _accounting.reset(new AccountingDb(_cfg));
  _statistics.reset(new StatisticDb(_cfg));

  coroutineCall(coroutineNew(msgHandlerProc, this, 0x100000));
  coroutineCall(coroutineNew(checkConfirmationsProc, this, 0x100000));  
  coroutineCall(coroutineNew(checkBalanceProc, this, 0x100000));      
  coroutineCall(coroutineNew(updateStatisticProc, this, 0x100000));
  coroutineCall(coroutineNew(payoutProc, this, 0x100000)); 
  
  LOG_F(INFO, "<info>: Pool backend for '%s' started, mode is %s", _cfg.CoinName.c_str(), _cfg.isMaster ? "MASTER" : "SLAVE");
  if (!_cfg.PoolFee.empty()) {
    for (const auto &poolFeeEntry: _cfg.PoolFee)
      LOG_F(INFO, "  Pool fee of %.2f to %s", poolFeeEntry.Percentage, poolFeeEntry.Address.c_str());
  } else {
    LOG_F(INFO, "  Pool fee disabled");
  }
  
  checkConsistency(_accounting.get());
  asyncLoop(_base);
}

void *PoolBackend::msgHandler()
{
  xmstream stream;
  while (true) {
    uint32_t msgSize;
    stream.reset();
    if (ioRead(_read, &msgSize, sizeof(msgSize), afWaitAll, 0) != sizeof(msgSize))
      break;
    if (ioRead(_read, stream.reserve(msgSize), msgSize, afWaitAll, 0) != msgSize)
      break;
    stream.seekSet(0);
    
    flatbuffers::Verifier verifier(stream.data<const uint8_t>(), stream.sizeOf());
    if (!VerifyP2PMessageBuffer(verifier)) {
      LOG_F(ERROR, " * pool backend error: can't decode message");
      continue;
    }
    
    const P2PMessage *msg = GetP2PMessage(stream.data());
    switch (msg->functionId()) {
      case FunctionId_Share :
        onShare(static_cast<const Share*>(msg->data()));
        break;
      case FunctionId_Stats :
        onStats(static_cast<const Stats*>(msg->data()));
        break;
      default :
        LOG_F(ERROR, " * pool backend error: unknown message type");
    }
  }

  return nullptr;
}

void *PoolBackend::checkConfirmationsHandler()
{
  aioUserEvent *timerEvent = newUserEvent(_base, 0, nullptr, nullptr);
  while (true) {
    ioSleep(timerEvent, _cfg.ConfirmationsCheckInterval);
    if (_client->connected()) {
      _accounting->cleanupRounds();
      _accounting->checkBlockConfirmations();
    } else {
      LOG_F(ERROR, "can't check block confirmations, no connection to wallet");
    }
  }
}


void *PoolBackend::payoutHandler()
{
  aioUserEvent *timerEvent = newUserEvent(_base, 0, nullptr, nullptr);
  while (true) {
    ioSleep(timerEvent, _cfg.PayoutInterval);
    if (_client->connected()) {
      _accounting->makePayout();
    } else {
      LOG_F(ERROR, "<error>: can't make payouts, no connection to wallet");
    }
  }
}

// Only for master
void *PoolBackend::checkBalanceHandler()
{
  aioUserEvent *timerEvent = newUserEvent(_base, 0, nullptr, nullptr);
  while (true) {
    ioSleep(timerEvent, _cfg.BalanceCheckInterval);
    if (_client->connected()) {
      _accounting->checkBalance();
    } else {
      LOG_F(ERROR, "<error>: can't check balance, no connection to wallet");
    }  
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
