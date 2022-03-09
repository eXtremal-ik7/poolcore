#pragma once

#include "poolcore/poolCore.h"
#include "poolcore/thread.h"
#include "asyncio/asyncio.h"
#include "asyncio/http.h"
#include "asyncio/socket.h"
#include <rapidjson/document.h>
#include <chrono>
#include <vector>
#include "loguru.hpp"

class CBitcoinRpcClient : public CNetworkClient {
public:
  CBitcoinRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, const char *login, const char *password, bool longPollEnabled);

  virtual CPreparedQuery *prepareBlock(const void *data, size_t size) override;
  virtual bool ioGetBalance(asyncBase *base, GetBalanceResult &result) override;
  virtual bool ioGetBlockConfirmations(asyncBase *base, std::vector<GetBlockConfirmationsQuery> &query) override;
  virtual EOperationStatus ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const int64_t value, BuildTransactionResult &result) override;
  virtual EOperationStatus ioSendTransaction(asyncBase *base, const std::string &txData, std::string &error) override;
  virtual EOperationStatus ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, std::string &error) override;
  virtual void aioSubmitBlock(asyncBase *base, CPreparedQuery *queryPtr, CSubmitBlockOperation *operation) override;
  virtual EOperationStatus ioListUnspent(asyncBase *base, ListUnspentResult &result) override;
  virtual EOperationStatus ioZSendMany(asyncBase *base, const std::string &source, const std::string &destination, int64_t amount, const std::string &memo, uint64_t minConf, int64_t fee, CNetworkClient::ZSendMoneyResult &result) override;
  virtual EOperationStatus ioZGetBalance(asyncBase *base, const std::string &address, int64_t *balance) override;

  virtual bool ioGetBlockExtraInfo(asyncBase*, std::vector<GetBlockExtraInfoQuery>&) override {
    return false;
  }
  virtual void poll() override;

private:
  struct GBTInstance {
    HTTPClient *Client;
    HTTPParseDefaultContext ParseCtx;
    std::string LongPollId;
    uint64_t WorkId;
    std::chrono::time_point<std::chrono::steady_clock> LastTemplateTime;
    aioUserEvent *TimerEvent;
  };

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
    int LastErrorCode = 0;
  };

  struct CPreparedSubmitBlock : public CPreparedQuery {
    CPreparedSubmitBlock(CBitcoinRpcClient *client) : CPreparedQuery(client) {}
    asyncBase *Base;
    CSubmitBlockOperation *Operation;
    std::unique_ptr<CConnection> Connection;
  };


private:
  std::string buildSendToAddress(const std::string &destination, int64_t amount);
  std::string buildGetTransaction(const std::string &txId);
  EOperationStatus signRawTransaction(CConnection *connection, const std::string &fundedTransaction, std::string &signedTransaction, std::string &error);

  void submitBlockRequestCb(CPreparedSubmitBlock *query) {
    std::unique_ptr<CPreparedSubmitBlock> queryHolder(query);
    bool result = false;
    rapidjson::Document document;
    if (parseJson(*query->Connection, document)) {
      if (document["result"].IsNull()) {
        result = true;
      } else if (document["result"].IsString() && query->Connection->LastError.empty()) {
        query->Connection->LastError = document["result"].GetString();
      }
    }

    query->Operation->accept(result, FullHostName_, query->Connection->LastError);
  }

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

  template<rapidjson::ParseFlag flag = rapidjson::kParseDefaultFlags>
  EOperationStatus ioQueryJson(CConnection &connection, const std::string &query, rapidjson::Document &document, uint64_t timeout) {
    AsyncOpStatus status = ioHttpRequest(connection.Client, query.data(), query.size(), timeout, httpParseDefault, &connection.ParseCtx);
    if (status != aosSuccess) {
      LOG_F(WARNING, "%s %s: error code: %u", CoinInfo_.Name.c_str(), FullHostName_.c_str(), status);
      return status == aosTimeout ? EStatusTimeout : EStatusNetworkError;
    }

    document.Parse<flag>(connection.ParseCtx.body.data, connection.ParseCtx.body.size);

    if (connection.ParseCtx.resultCode != 200) {
      if (!document.HasParseError()) {
        if (document.HasMember("error") && document["error"].IsObject()) {
          rapidjson::Value &value = document["error"];
          if (value.HasMember("code") && value["code"].IsInt())
            connection.LastErrorCode = value["code"].GetInt();
          if (value.HasMember("message") && value["message"].IsString())
            connection.LastError = value["message"].GetString();
        }
      }

      LOG_F(WARNING, "%s %s: request error code: %u (http result code: %u, data: %s)",
            CoinInfo_.Name.c_str(),
            FullHostName_.c_str(),
            static_cast<unsigned>(status),
            connection.ParseCtx.resultCode,
            connection.ParseCtx.body.data ? connection.ParseCtx.body.data : "<null>");
      return EStatusProtocolError;
    }

    if (document.HasParseError()) {
      LOG_F(WARNING, "%s %s: JSON parse error", CoinInfo_.Name.c_str(), FullHostName_.c_str());
      return EStatusProtocolError;
    }

    return EStatusOk;
  }

  void onWorkFetcherConnect(AsyncOpStatus status);
  void onWorkFetcherIncomingData(AsyncOpStatus status);
  void onWorkFetchTimeout();

  CConnection *getConnection(asyncBase *base);

private:
  asyncBase *WorkFetcherBase_;
  unsigned ThreadsNum_;
  CCoinInfo CoinInfo_;

  HostAddress Address_;
  std::string HostName_;
  std::string FullHostName_;
  std::string BasicAuth_;

  GBTInstance WorkFetcher_;
  bool HasLongPoll_;
  bool HasGetWalletInfo_ = true;
  bool HasGetBlockChainInfo_ = true;
  bool HasSignRawTransactionWithWallet_ = true;

  // Queries cache
  std::string BalanceQuery_;
  std::string BalanceQueryWithImmatured_;
  std::string GetWalletInfoQuery_;
};
