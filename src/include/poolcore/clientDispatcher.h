#pragma once

#include "poolCore.h"
#include "poolInstance.h"
#include "loguru.hpp"
#include <memory>

class CNetworkClientDispatcher {
public:
  CNetworkClientDispatcher(asyncBase *base, const CCoinInfo &coinInfo, unsigned threadsNum) : Base_(base), CoinInfo_(coinInfo) {
    CurrentClientIdx_.resize(threadsNum, 0);
    WorkFetcherReconnectTimer_ = newUserEvent(base, 0, [](aioUserEvent*, void *arg) {
      static_cast<CNetworkClientDispatcher*>(arg)->onWorkFetchReconnectTimer();
    }, this);
  }
  void addClient(CNetworkClient *client) {
    Clients_.emplace_back(client);
    client->setDispatcher(this);
  }

  void setBackend(PoolBackend *backend) { Backend_ = backend; }

  // Common API
  bool ioGetBalance(asyncBase *base, CNetworkClient::GetBalanceResult &result);
  bool ioGetBlockConfirmations(asyncBase *base, const std::vector<std::string> &hashes, std::vector<int64_t> &result);
  bool ioListUnspent(asyncBase *base, CNetworkClient::ListUnspentResult &result);
  bool ioSendMoney(asyncBase *base, const char *address, int64_t value, CNetworkClient::SendMoneyResult &result);
  void aioSubmitBlockPrepared(asyncBase *base, const std::string *preparedData, void *callback);
  void aioSubmitBlock(asyncBase *base, const std::string &block, void *callback);

  // ZEC specific
  bool ioZGetBalance(asyncBase *base, int64_t *result);
  bool ioZSendMoney(asyncBase *base, const std::string &source, const std::string &destination, int64_t amount, const std::string &memo, CNetworkClient::ZSendMoneyResult &result);

  // Work polling
  void connectWith(CPoolInstance *instance) { LinkedInstances_.push_back(instance); }
  void poll();

  void onWorkFetchReconnectTimer();
  void onWorkFetcherConnectionError();
  void onWorkFetcherConnectionLost();
  void onWorkFetcherNewWork(rapidjson::Value &work);

private:
  enum EWorkState {
    EWorkOk = 0,
    EWorkLost,
    EWorkMinersStopped
  };

private:
  asyncBase *Base_;
  CCoinInfo CoinInfo_;
  PoolBackend *Backend_ = nullptr;
  std::vector<std::unique_ptr<CNetworkClient>> Clients_;
  std::vector<size_t> CurrentClientIdx_;
  size_t CurrentWorkFetcherIdx = 0;

  std::vector<CPoolInstance*> LinkedInstances_;

  aioUserEvent *WorkFetcherReconnectTimer_ = nullptr;
  EWorkState WorkState_ = EWorkOk;
  std::chrono::time_point<std::chrono::steady_clock> ConnectionLostTime_;
};
