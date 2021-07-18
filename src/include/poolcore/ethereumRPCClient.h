#pragma once

#include "poolcore/poolCore.h"
#include "asyncio/http.h"
#include "asyncio/socket.h"
#include <rapidjson/document.h>
#include "loguru.hpp"

struct PoolBackendConfig;

class CEthereumRpcClient : public CNetworkClient {
public:
  CEthereumRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, PoolBackendConfig &config);

  virtual CPreparedQuery *prepareBlock(const void *data, size_t) override;
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

  struct CPreparedSubmitBlock : public CPreparedQuery {
    CPreparedSubmitBlock(CEthereumRpcClient *client) : CPreparedQuery(client) {}
    asyncBase *Base;
    CSubmitBlockOperation *Operation;
    std::unique_ptr<CConnection> Connection;
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
  template<rapidjson::ParseFlag flag = rapidjson::kParseDefaultFlags>
  bool parseJson(CConnection &connection, rapidjson::Document &document) {
    document.Parse<flag>(connection.ParseCtx.body.data, connection.ParseCtx.body.size);

    if (connection.ParseCtx.resultCode != 200) {
      if (!document.HasParseError()) {
        if (document.HasMember("error") && document["error"].IsObject()) {
          rapidjson::Value &value = document["error"];
          if (value.HasMember("message") && value["message"].IsString())
            connection.LastError = value["message"].GetString();
        }
      }

      LOG_F(WARNING, "%s %s: http result code: %u, data: %s",
            CoinInfo_.Name.c_str(),
            FullHostName_.c_str(),
            connection.ParseCtx.resultCode,
            connection.ParseCtx.body.data ? connection.ParseCtx.body.data : "<null>");
      return false;
    }

    if (document.HasParseError()) {
      LOG_F(WARNING, "%s %s: JSON parse error", CoinInfo_.Name.c_str(), FullHostName_.c_str());
      return false;
    }

    if (!document.HasMember("result")) {
      LOG_F(WARNING, "%s %s: JSON: no 'result' object", CoinInfo_.Name.c_str(), FullHostName_.c_str());
      return false;
    }

    return true;
  }

  void submitBlockRequestCb(CPreparedSubmitBlock *query) {
    std::unique_ptr<CPreparedSubmitBlock> queryHolder(query);
    bool result = false;
    rapidjson::Document document;
    if (parseJson(*query->Connection, document)) {
      if (document["result"].IsTrue()) {
        result = true;
      } else {
        query->Connection->LastError = "?";
      }
    }

    query->Operation->accept(result, FullHostName_, query->Connection->LastError);
  }

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
