#pragma once

#include "poolcore/poolCore.h"
#include "asyncio/http.h"
#include "asyncio/socket.h"
#include "loguru.hpp"

struct PoolBackendConfig;

class CEthereumRpcClient : public CNetworkClient {
public:
  CEthereumRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, PoolBackendConfig &config);

  virtual CPreparedQuery *prepareBlock(const void *data, size_t size) override;
  virtual bool ioGetBalance(asyncBase *base, GetBalanceResult &result) override;
  virtual bool ioGetBlockConfirmations(asyncBase *base, std::vector<GetBlockConfirmationsQuery> &query) override;
  virtual EOperationStatus ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const int64_t value, BuildTransactionResult &result) override;
  virtual EOperationStatus ioSendTransaction(asyncBase *base, const std::string &txData, std::string &error) override;
  virtual EOperationStatus ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, std::string &error) override;
  virtual void aioSubmitBlock(asyncBase *base, CPreparedQuery *queryPtr, CSubmitBlockOperation *operation) override;

  virtual EOperationStatus ioListUnspent(asyncBase*, ListUnspentResult&) final {
    return CNetworkClient::EOperationStatus::EStatusUnknownError;
  }
  virtual EOperationStatus ioZSendMany(asyncBase*, const std::string&, const std::string&, int64_t, const std::string&, uint64_t, int64_t, CNetworkClient::ZSendMoneyResult&) final {
    return CNetworkClient::EOperationStatus::EStatusUnknownError;
  }
  virtual EOperationStatus ioZGetBalance(asyncBase*, const std::string&, int64_t*) final {
    return CNetworkClient::EOperationStatus::EStatusUnknownError;
  }

  virtual void poll() override;


private:
  struct CConnection {
    CConnection() : Client(nullptr) {
      httpParseDefaultInit(&ParseCtx);
    }

    ~CConnection() {
      dynamicBufferFree(&ParseCtx.buffer);
      if (Client)
        httpClientDelete(Client);
    }

    CConnection(const CConnection&) = delete;
    CConnection(CConnection&&) = default;

    socketTy Socket;
    HTTPClient *Client = nullptr;
    HTTPParseDefaultContext ParseCtx;
    std::string LastError;
  };

  struct WorkFetcherContext {
    HTTPClient *Client;
    HTTPParseDefaultContext ParseCtx;
    std::chrono::time_point<std::chrono::steady_clock> LastTemplateTime;
    aioUserEvent *TimerEvent;
    uint64_t WorkId;
    uint64_t Height;
  };

private:
  void onWorkFetcherConnect(AsyncOpStatus status);
  void onWorkFetcherIncomingData(AsyncOpStatus status);
  void onWorkFetchTimeout();

  CConnection *getConnection(asyncBase *base);

private:
  asyncBase *WorkFetcherBase_ = nullptr;
  unsigned ThreadsNum_;
  CCoinInfo CoinInfo_;
  std::string HostName_;
  std::string FullHostName_;
  HostAddress Address_;

  WorkFetcherContext WorkFetcher_;

  xmstream EthGetWork_;
};
