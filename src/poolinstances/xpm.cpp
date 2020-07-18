#include "poolinstances/xpm.h"
#include "asyncio/socket.h"
#include "loguru.hpp"


static void createListener(uint16_t port)
{
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = htons(port);
  socketTy hSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  socketReuseAddr(hSocket);
  if (socketBind(hSocket, &address) != 0) {
    LOG_F(ERROR, "cannot bind port: %i", port);
    exit(1);
  }
}

XpmZmqInstance::XpmZmqInstance(unsigned workersNum, CPoolThread *workers, const std::string &name, rapidjson::Value &config) :
  CPoolInstance(workersNum, workers), Name_(name)
{
  Data_.reset(new ThreadData[workersNum]);
  if (!(config.HasMember("port") && config["port"].IsInt() &&
        config.HasMember("workerPort") && config["workerPort"].IsInt())) {
    LOG_F(ERROR, "instance %s: can't read 'port' and 'workerPort' values from config", "XPM/zmq");
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

void XpmZmqInstance::checkNewBlockTemplate(rapidjson::Value &blockTemplate)
{

}

void XpmZmqInstance::acceptNewConnection(unsigned workerId, aioObject *socket)
{

}

void XpmZmqInstance::acceptNewWork(unsigned workerId, intrusive_ptr<CWorkInstance> work)
{

}
