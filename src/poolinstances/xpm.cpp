#include "poolcore/backend.h"
#include "poolinstances/xpm.h"
#include "blockmaker/script.h"
#include "asyncio/socket.h"
#include "loguru.hpp"







XpmZmqInstance::XpmZmqInstance(unsigned workersNum, CPoolThread *workers, rapidjson::Value &config) :
  CPoolInstance(workersNum, workers), SerializeBuffer_(4u << 20)
{
  Name_ = "XPM.zmq";
  Data_.reset(new ThreadData[workersNum]);
  if (!(config.HasMember("port") && config["port"].IsInt() &&
        config.HasMember("workerPort") && config["workerPort"].IsInt() &&
        config.HasMember("miningAddress") && config["miningAddress"].IsString())) {
    LOG_F(ERROR, "instance %s: can't read 'port', 'workerPort', 'miningAddress' values from config", "XPM/zmq");
    exit(1);
  }

  uint16_t port = config["port"].GetInt();
  uint16_t workerPort = config["workerPort"].GetInt();
  createListener(port);
  createListener(workerPort);
  createListener(workerPort+1);
}

void XpmZmqInstance::stopWork()
{

}

void XpmZmqInstance::checkNewBlockTemplate(rapidjson::Value &blockTemplate, PoolBackend *backend)
{
  XPMWorkInstance *work = new XPMWorkInstance;
  intrusive_ptr<CWorkInstance> workHolder(work);

  // Fill block with header fields and transactions
  // Header fields
  if (!loadHeader<XPM::Proto>(work->block, blockTemplate)) {
    LOG_F(WARNING, "%s: Malformed block template (can't parse transactions)", Name_.c_str());
    return;
  }

  // Transactions
  if (!loadTransactions<XPM::Proto>(work->block, blockTemplate, SerializeBuffer_)) {
    LOG_F(WARNING, "%s: Malformed block template (can't parse transactions)", Name_.c_str());
    return;
  }

  // Build coinbase
  const PoolBackendConfig &cfg = backend->getConfig();
  const CCoinInfo &info = backend->getCoinInfo();
  BTC::Proto::AddressTy miningAddress;
  if (!decodeHumanReadableAddress(cfg.MiningAddress, info.PubkeyAddressPrefix, miningAddress)) {
    LOG_F(WARNING, "%s: mining address %s is invalid", cfg.MiningAddress.c_str());
    return;
  }

  bool coinBaseSuccess = info.SegwitEnabled ?
    buildSegwitCoinbase<XPM::Proto>(work->block.vtx[0], miningAddress, blockTemplate) :
    buildCoinbase<XPM::Proto>(work->block.vtx[0], miningAddress, blockTemplate);
  if (!coinBaseSuccess) {
    LOG_F(WARNING, "%s: Insufficient data for coinbase transaction in block template", Name_.c_str());
    return;
  }
}

void XpmZmqInstance::acceptNewConnection(unsigned workerId, aioObject *socket)
{

}

void XpmZmqInstance::acceptNewWork(unsigned workerId, intrusive_ptr<CWorkInstance> work)
{

}
