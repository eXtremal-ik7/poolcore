#include "api.h"
#include "poolcore/backend.h"
#include "asyncio/coroutine.h"
#include "p2p/p2p.h"
#include "p2putils/xmstream.h"

#include "boost/bind.hpp"
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
      userBalance balance;
      RawData data = It->value();
      if (!balance.deserializeValue(data.data, data.size))
        break;      
        
      balancesRequested[balance.userId] = balance.requested;
      totalInBalance += balance.requested;
    }
  }
  
  LOG_F(INFO, "totalQueued: %li", totalQueued);
  LOG_F(INFO, "totalRequested: %li", totalInBalance);
}


PoolBackend::PoolBackend(config *cfg) : _cfg(*cfg)
{
  _base = createAsyncBase(amOSDefault);
  _timeout = 8*1000000;
  pipeCreate(&_pipeFd, 1);
  _read = newDeviceIo(_base, _pipeFd.read);
  _write = newDeviceIo(_base, _pipeFd.write);
}

PoolBackend::~PoolBackend()
{
  delete _accounting;
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
  _thread = new boost::thread(boost::bind(&PoolBackend::backendMain, boost::ref(*this)));
}

void PoolBackend::stop()
{
  postQuitOperation(_base);
  _thread->join();
}


void *PoolBackend::backendMain()
{
  if (_cfg.useAsyncPayout && (_cfg.poolZAddr.empty() || _cfg.poolTAddr.empty())) {
    LOG_F(ERROR, "<error> pool configured to use async payouts(like ZCash) but not specified pool Z-Addr/T-Addr");
    exit(1);
  }
  
  _client = p2pNode::createClient(_base, &_cfg.peers[0], _cfg.peers.size(), _cfg.walletAppName.c_str());  
  
  
  AccountingDb::config accountingCfg;
  accountingCfg.poolFee = _cfg.poolFee;
  accountingCfg.poolFeeAddr = _cfg.poolFeeAddr;
  accountingCfg.requiredConfirmations = _cfg.requiredConfirmations;
  accountingCfg.defaultMinimalPayout = _cfg.defaultMinimalPayout;
  accountingCfg.minimalPayout = _cfg.minimalPayout;
  accountingCfg.dbPath = _cfg.dbPath;
  accountingCfg.keepRoundTime = _cfg.keepRoundTime;
  accountingCfg.poolZAddr = _cfg.poolZAddr;
  accountingCfg.poolTAddr = _cfg.poolTAddr;
  accountingCfg.checkAddressProc = _cfg.checkAddressProc;  
  _accounting = new AccountingDb(&accountingCfg, _client);  
  
  StatisticDb::config statisticsCfg;
  statisticsCfg.dbPath = _cfg.dbPath;
  statisticsCfg.keepStatsTime = _cfg.keepStatsTime;
  _statistics = new StatisticDb(&statisticsCfg, _client);
  
  _node = p2pNode::createNode(_base, &_cfg.listenAddress, _cfg.poolAppName.c_str(), true);
  _node->setRequestHandler(poolcoreRequestHandler, this);  

  coroutineCall(coroutineNew(msgHandlerProc, this, 0x100000));
  coroutineCall(coroutineNew(checkConfirmationsProc, this, 0x100000));  
  coroutineCall(coroutineNew(checkBalanceProc, this, 0x100000));      
  coroutineCall(coroutineNew(updateStatisticProc, this, 0x100000));
  coroutineCall(coroutineNew(payoutProc, this, 0x100000)); 
  
  LOG_F(INFO, "<info>: Pool backend started, mode is %s", _cfg.isMaster ? "MASTER" : "SLAVE");
  if (_cfg.poolFee)
    LOG_F(INFO, "<info>: Pool fee of %u%% to %s", _cfg.poolFee, _cfg.poolFeeAddr.c_str());
  else
    LOG_F(INFO, "<info>: Pool fee disabled");
  
  checkConsistency(_accounting);
  asyncLoop(_base);
  
  delete _statistics;
  delete _accounting;
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
}

void *PoolBackend::checkConfirmationsHandler()
{
  aioUserEvent *timerEvent = newUserEvent(_base, 0, nullptr, nullptr);
  while (true) {
    ioSleep(timerEvent, _cfg.confirmationsCheckInterval);
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
    ioSleep(timerEvent, _cfg.payoutInterval);
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
    ioSleep(timerEvent, _cfg.balanceCheckInterval);  
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
    ioSleep(timerEvent, _cfg.statisticCheckInterval);
    _statistics->update();
  }
}

void PoolBackend::onShare(const Share *share)
{
  _accounting->addShare(share, _statistics);
}

void PoolBackend::onStats(const Stats *stats)
{
  _statistics->addStats(stats);
}
