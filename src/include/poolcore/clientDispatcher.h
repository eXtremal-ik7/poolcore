#pragma once

#include "poolCore.h"
#include "poolInstance.h"
#include "loguru.hpp"
#include <memory>

class CNetworkClientDispatcher {  
public:
  CNetworkClientDispatcher(asyncBase *base, const CCoinInfo &coinInfo, unsigned threadsNum) : CoinInfo_(coinInfo) {
    CurrentClientIdx_.resize(threadsNum, 0);
    WorkFetcherReconnectTimer_ = newUserEvent(base, 0, [](aioUserEvent*, void *arg) {
      static_cast<CNetworkClientDispatcher*>(arg)->onWorkFetchReconnectTimer();
    }, this);
  }
  void addGetWorkClient(CNetworkClient *client) {
    GetWorkClients_.emplace_back(client);
    client->setDispatcher(this);
  }

  void addRPCClient(CNetworkClient *client) {
    RPCClients_.emplace_back(client);
    client->setDispatcher(this);
  }

  PoolBackend *backend() { return Backend_; }
  void setBackend(PoolBackend *backend) { Backend_ = backend; }

  // Common API
  bool ioGetBalance(asyncBase *base, CNetworkClient::GetBalanceResult &result);
  bool ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<CNetworkClient::GetBlockConfirmationsQuery> &query);
  bool ioGetBlockExtraInfo(asyncBase *base, int64_t orphanAgeLimit, std::vector<CNetworkClient::GetBlockExtraInfoQuery> &query);
  CNetworkClient::EOperationStatus ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const int64_t value, CNetworkClient::BuildTransactionResult &result);
  CNetworkClient::EOperationStatus ioSendTransaction(asyncBase *base, const std::string &txData, const std::string &txId, std::string &error);
  CNetworkClient::EOperationStatus ioWalletService(asyncBase *base, std::string &error);
  CNetworkClient::EOperationStatus ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, int64_t *txFee, std::string &error);
  void aioSubmitBlock(asyncBase *base, const void *data, size_t size, CNetworkClient::SumbitBlockCb callback);

  // ZEC specific
  CNetworkClient::EOperationStatus ioListUnspent(asyncBase *base, CNetworkClient::ListUnspentResult &result);
  CNetworkClient::EOperationStatus ioZGetBalance(asyncBase *base, const std::string &address, int64_t *result);
  CNetworkClient::EOperationStatus ioZSendMoney(asyncBase *base, const std::string &source, const std::string &destination, int64_t amount, const std::string &memo, uint64_t minConf, int64_t fee, CNetworkClient::ZSendMoneyResult &result);

  void connectWith(CPoolInstance *instance) {
    LinkedInstances_.push_back(instance);
    instance->addBackend(Backend_);
  }

  // Work polling
  void poll();

  void onWorkFetchReconnectTimer();
  void onWorkFetcherConnectionError();
  void onWorkFetcherConnectionLost();
  void onWorkFetcherNewWork(CBlockTemplate *blockTemplate);

private:
  enum EWorkState {
    EWorkOk = 0,
    EWorkLost,
    EWorkMinersStopped
  };

private:
  CCoinInfo CoinInfo_;
  PoolBackend *Backend_ = nullptr;
  std::vector<std::unique_ptr<CNetworkClient>> GetWorkClients_;
  std::vector<std::unique_ptr<CNetworkClient>> RPCClients_;
  std::vector<size_t> CurrentClientIdx_;
  size_t CurrentWorkFetcherIdx = 0;

  std::vector<CPoolInstance*> LinkedInstances_;

  aioUserEvent *WorkFetcherReconnectTimer_ = nullptr;
  EWorkState WorkState_ = EWorkOk;
  std::chrono::time_point<std::chrono::steady_clock> ConnectionLostTime_;
};
