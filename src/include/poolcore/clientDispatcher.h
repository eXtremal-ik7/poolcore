#pragma once

#include "poolCore.h"
#include "poolInstance.h"
#include "loguru.hpp"
#include <memory>

class CNetworkClientDispatcher {
public:
  struct GetBalanceResult {
    int64_t Balance;
    int64_t Immatured;
  };

  struct SendMoneyResult {
    bool Success;
    std::string TxId;
    std::string Error;
    int64_t Remaining;
    int64_t Fee;
  };

  struct ListUnspentElement {
    std::string Address;
    int64_t Amount;
  };

  struct ListUnspentResult {
    std::vector<ListUnspentElement> Outs;
  };

  struct ZSendMoneyResult {
    std::string AsyncOperationId;
    std::string Error;
  };

public:
  CNetworkClientDispatcher(asyncBase *base, const CCoinInfo &coinInfo) : Base_(base), CoinInfo_(coinInfo) {
    WorkFetcherReconnectTimer_ = newUserEvent(base, 0, [](aioUserEvent*, void *arg) {
      static_cast<CNetworkClientDispatcher*>(arg)->onWorkFetchReconnectTimer();
    }, this);
  }
  void addClient(CNetworkClient *client) {
    Clients_.emplace_back(client);
    client->setDispatcher(this);
  }

  bool ioGetBalance(GetBalanceResult &result);
  bool ioGetBlockConfirmations(const std::vector<std::string> &hashes, std::vector<int64_t> &result);
  bool ioListUnspent(ListUnspentResult &result);
  bool ioSendMoney(const char *address, int64_t value, SendMoneyResult &result);

  // ZEC specific
  bool ioZGetBalance(int64_t *result);
  bool ioZSendMoney(const std::string &source, const std::string &destination, int64_t amount, const std::string &memo, ZSendMoneyResult &result);

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
  std::vector<std::unique_ptr<CNetworkClient>> Clients_;
  std::vector<CPoolInstance*> LinkedInstances_;
  size_t CurrentWorkFetcherIdx = 0;
  size_t CurrentClientIdx = 0;

  aioUserEvent *WorkFetcherReconnectTimer_ = nullptr;
  EWorkState WorkState_ = EWorkOk;
  std::chrono::time_point<std::chrono::steady_clock> ConnectionLostTime_;
};
