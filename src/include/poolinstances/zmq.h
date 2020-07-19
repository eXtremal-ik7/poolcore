#pragma once

#include "common.h"
#include "poolcore/backend.h"
#include "poolcore/poolCore.h"
#include "poolcore/poolInstance.h"
#include "blockmaker/xpm.h"
#include <unordered_map>

//static void createListener(uint16_t port)
//{
//  HostAddress address;
//  address.family = AF_INET;
//  address.ipv4 = INADDR_ANY;
//  address.port = htons(port);
//  socketTy hSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
//  socketReuseAddr(hSocket);
//  if (socketBind(hSocket, &address) != 0) {
//    LOG_F(ERROR, "cannot bind port: %i", port);
//    exit(1);
//  }
//}

template<typename Proto>
class ZmqInstance : public CPoolInstance {
public:
  ZmqInstance(unsigned workersNum, CPoolThread *workers, rapidjson::Value &config) : CPoolInstance(workersNum, workers)  {
    Name_ = (std::string)Proto::TickerName + ".zmq";
    Data_.reset(new ThreadData[workersNum]);
    if (!(config.HasMember("port") && config["port"].IsInt() &&
          config.HasMember("workerPort") && config["workerPort"].IsInt())) {
      LOG_F(ERROR, "instance %s: can't read 'port', 'workerPort', 'miningAddress' values from config", "XPM/zmq");
      exit(1);
    }

    uint16_t port = config["port"].GetInt();
    uint16_t workerPort = config["workerPort"].GetInt();
  }

  virtual void checkNewBlockTemplate(rapidjson::Value &blockTemplate, PoolBackend *backend) override {
    intrusive_ptr<CWorkInstance> work = ::checkNewBlockTemplate<Proto>(blockTemplate, backend->getConfig(), backend->getCoinInfo(), SerializeBuffer_, Name_);
    if (!work.get())
      return;

    for (unsigned i = 0; i < WorkersNum_; i++)
      Workers_[i].newWork(*this, work);
  }

  virtual void acceptNewConnection(unsigned workerId, aioObject *socket) override {

  }

  virtual void acceptNewWork(unsigned workerId, intrusive_ptr<CWorkInstance> work) override {
    LOG_F(INFO, "Work received for %s", Proto::TickerName);
  }

private:
  struct ThreadData {
    typename Proto::Block block;
  };

private:
  std::unique_ptr<ThreadData[]> Data_;

  std::string Name_;
  xmstream SerializeBuffer_;
};
