#pragma once

#include "poolcore/poolCore.h"
#include "poolcore/thread.h"
#include "asyncio/asyncio.h"
#include "asyncio/http.h"
#include "asyncio/socket.h"
#include <chrono>
#include <vector>
#include "loguru.hpp"

struct HTTPClient;

class CBitcoinRpcClient : public CNetworkClient {
public:
  CBitcoinRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, const char *login, const char *password);

  virtual CPreparedQuery *prepareBlock(const void *data, size_t size) override;
  virtual bool ioGetBalance(asyncBase *base, GetBalanceResult &result) override;
  virtual bool ioSendMoney(asyncBase *base, const char *address, int64_t value, CNetworkClient::SendMoneyResult &result) override;
  virtual void aioSubmitBlock(asyncBase *base, CPreparedQuery *queryPtr, SumbitBlockCb callback) override;

  virtual void poll() override;

private:
  struct GBTInstance {
    HTTPClient *Client;
    HTTPParseDefaultContext ParseCtx;
    std::string LongPollId;
    std::string PreviousBlock;
    std::chrono::time_point<std::chrono::steady_clock> LastTemplateTime;
    aioUserEvent *TimerEvent;
  };

  struct CConnection {
    CConnection() : Client(nullptr) {}
    CConnection(const CConnection&) = delete;
    CConnection(CConnection&&) = default;

    socketTy Socket;
    HTTPClient *Client = nullptr;
    HTTPParseDefaultContext ParseCtx;
    std::string LastError;

    ~CConnection() {
      if (Client)
        httpClientDelete(Client);
    }
  };

  struct CPreparedSubmitBlock : public CPreparedQuery {
    CPreparedSubmitBlock(CBitcoinRpcClient *client) : CPreparedQuery(client) {}
    asyncBase *Base;
    SumbitBlockCb Callback;
    std::unique_ptr<CConnection> Connection;
  };


private:
  std::string buildSendToAddress(const std::string &destination, int64_t amount);
  std::string buildGetTransaction(const std::string &txId);

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

    query->Callback(result, query->Connection->LastError);
  }

  template<rapidjson::ParseFlag flag = rapidjson::kParseDefaultFlags>
  bool parseJson(CConnection &connection, rapidjson::Document &document) {
    document.Parse<flag>(connection.ParseCtx.body.data);

    if (connection.ParseCtx.resultCode != 200) {
      if (!document.HasParseError()) {
        if (document.HasMember("error") && document["error"].IsObject()) {
          rapidjson::Value &value = document["error"];
          if (value.HasMember("message") && value["message"].IsString())
            connection.LastError = value["message"].GetString();
        }
      }

      LOG_F(WARNING, "%s %s:%u: http result code: %u, data: %s",
            CoinInfo_.Name.c_str(),
            HostName_.c_str(),
            static_cast<unsigned>(htons(Address_.port)),
            connection.ParseCtx.resultCode,
            connection.ParseCtx.body.data ? connection.ParseCtx.body.data : "<null>");
      return false;
    }

    if (document.HasParseError()) {
      LOG_F(WARNING, "%s %s:%u: JSON parse error", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
      return false;
    }

    if (!document.HasMember("result")) {
      LOG_F(WARNING, "%s %s:%u: JSON: no 'result' object", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
      return false;
    }

    return true;
  }

  template<rapidjson::ParseFlag flag = rapidjson::kParseDefaultFlags>
  bool ioQueryJson(CConnection &connection, const std::string &query, rapidjson::Document &document, uint64_t timeout) {
    AsyncOpStatus status = ioHttpRequest(connection.Client, query.data(), query.size(), timeout, httpParseDefault, &connection.ParseCtx);
    if (status != aosSuccess) {
      LOG_F(WARNING, "%s %s:%u: error code: %u", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)), status);
      return false;
    }

    document.Parse<flag>(connection.ParseCtx.body.data);

    if (connection.ParseCtx.resultCode != 200) {
      if (!document.HasParseError()) {
        if (document.HasMember("error") && document["error"].IsObject()) {
          rapidjson::Value &value = document["error"];
          if (value.HasMember("message") && value["message"].IsString())
            connection.LastError = value["message"].GetString();
        }
      }

      LOG_F(WARNING, "%s %s:%u: request error code: %u (http result code: %u, data: %s)",
            CoinInfo_.Name.c_str(),
            HostName_.c_str(),
            static_cast<unsigned>(htons(Address_.port)),
            static_cast<unsigned>(status),
            connection.ParseCtx.resultCode,
            connection.ParseCtx.body.data ? connection.ParseCtx.body.data : "<null>");
      return false;
    }

    if (document.HasParseError()) {
      LOG_F(WARNING, "%s %s:%u: JSON parse error", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
      return false;
    }

    if (!document.HasMember("result")) {
      LOG_F(WARNING, "%s %s:%u: JSON: no 'result' object", CoinInfo_.Name.c_str(), HostName_.c_str(), static_cast<unsigned>(htons(Address_.port)));
      return false;
    }

    return true;
  }

  void onWorkFetcherConnect(AsyncOpStatus status);
  void onWorkFetcherIncomingData(AsyncOpStatus status);
  void onWorkFetchTimeout();
  void onClientRequestTimeout();

  CConnection *getConnection(asyncBase *base);

private:
  asyncBase *WorkFetcherBase_;
  unsigned ThreadsNum_;
  CCoinInfo CoinInfo_;

  HostAddress Address_;
  std::string HostName_;
  std::string BasicAuth_;

  GBTInstance WorkFetcher_;
  bool HasGetWalletInfo_;

  // Queries cache
  std::string BalanceQuery_;
  std::string BalanceQueryWithImmatured_;
  std::string GetWalletInfoQuery_;
};