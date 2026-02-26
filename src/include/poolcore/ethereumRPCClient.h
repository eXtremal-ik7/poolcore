#pragma once

#include "poolcore/poolCore.h"
#include "poolcommon/uint.h"
#include "asyncio/http.h"
#include "asyncio/socket.h"
#include <rapidjson/document.h>
#include "loguru.hpp"
#include <chrono>

struct PoolBackendConfig;

class CEthereumRpcClient : public CNetworkClient {
public:
  CEthereumRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, PoolBackendConfig &config);

  virtual CPreparedQuery *prepareBlock(const void *data, size_t) override;
  virtual bool ioGetBalance(asyncBase *base, GetBalanceResult &result) override;
  virtual bool ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockConfirmationsQuery> &queries) override;
  virtual bool ioGetBlockExtraInfo(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockExtraInfoQuery> &queries) override;
  virtual EOperationStatus ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const UInt<384> &value, BuildTransactionResult &result) override;
  virtual EOperationStatus ioSendTransaction(asyncBase *base, const std::string &txData, const std::string &txId, std::string &error) override;
  virtual EOperationStatus ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, UInt<384> *txFee, std::string &error) override;
  virtual EOperationStatus ioWalletService(asyncBase *base, std::string &error) override;
  virtual void aioSubmitBlock(asyncBase *base, CPreparedQuery *queryPtr, CSubmitBlockOperation *operation) override;

  virtual EOperationStatus ioListUnspent(asyncBase*, ListUnspentResult&) final {
    return CNetworkClient::EOperationStatus::EStatusUnknownError;
  }
  virtual EOperationStatus ioZSendMany(asyncBase*, const std::string&, const std::string&, const UInt<384>&, const std::string&, uint64_t, const UInt<384>&, CNetworkClient::ZSendMoneyResult&) final {
    return CNetworkClient::EOperationStatus::EStatusUnknownError;
  }
  virtual EOperationStatus ioZGetBalance(asyncBase*, const std::string&, UInt<384>*) final {
    return CNetworkClient::EOperationStatus::EStatusUnknownError;
  }

  virtual bool ioGetBlockTxFees(asyncBase*, int64_t, int64_t, std::vector<BlockTxFeeInfo>&) override {
    return false;
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
    int LastErrorCode = 0;
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

      CLOG_F(WARNING, "{} {}: http result code: {}, data: {}",
             CoinInfo_.Name,
             FullHostName_,
             connection.ParseCtx.resultCode,
             connection.ParseCtx.body.data ? connection.ParseCtx.body.data : "<null>");
      return false;
    }

    if (document.HasParseError()) {
      CLOG_F(WARNING, "{} {}: JSON parse error", CoinInfo_.Name, FullHostName_);
      return false;
    }

    if (!document.HasMember("result")) {
      CLOG_F(WARNING, "{} {}: JSON: no 'result' object", CoinInfo_.Name, FullHostName_);
      return false;
    }

    return true;
  }

  template<rapidjson::ParseFlag flag = rapidjson::kParseDefaultFlags>
  EOperationStatus ioQueryJson(CConnection &connection, const std::string &query, rapidjson::Document &document, uint64_t timeout) {
    AsyncOpStatus status = ioHttpRequest(connection.Client, query.data(), query.size(), timeout, httpParseDefault, &connection.ParseCtx);
    if (status != aosSuccess) {
      CLOG_F(WARNING, "{} {}: error code: {}", CoinInfo_.Name, FullHostName_, static_cast<unsigned>(status));
      return status == aosTimeout ? EStatusTimeout : EStatusNetworkError;
    }

    if (connection.ParseCtx.resultCode != 200) {
      CLOG_F(WARNING, "{} {}: request error code: {} (http result code: {}, data: {})",
             CoinInfo_.Name,
             FullHostName_,
             static_cast<unsigned>(status),
             connection.ParseCtx.resultCode,
             connection.ParseCtx.body.data ? connection.ParseCtx.body.data : "<null>");
      return EStatusUnknownError;
    }

    document.Parse<flag>(connection.ParseCtx.body.data, connection.ParseCtx.body.size);
    if (document.HasParseError()) {
      CLOG_F(WARNING, "{} {}: JSON parse error", CoinInfo_.Name, FullHostName_);
      return EStatusProtocolError;
    }

    if (document.HasMember("error") && document["error"].IsObject()) {
      rapidjson::Value &value = document["error"];
      if (value.HasMember("code") && value["code"].IsInt())
        connection.LastErrorCode = value["code"].GetInt();
      if (value.HasMember("message") && value["message"].IsString())
        connection.LastError = value["message"].GetString();

      CLOG_F(WARNING, "{} {}: Error code: {}, Error message: {}",
             CoinInfo_.Name,
             FullHostName_,
             connection.LastErrorCode,
             connection.LastError);

      return EStatusProtocolError;
    }

    return EStatusOk;
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

  // Raw Ethereum API - structures
  struct ETHTransaction {
    UInt<384> GasPrice;
    UInt<256> Hash;
  };

  struct ETHTransactionReceipt {
    uint64_t BlockNumber;
    uint64_t GasUsed;
  };

  struct ETHBlock {
    UInt<256> MixHash;
    UInt<256> Hash;
    uint64_t GasUsed;
    UInt<384> BaseFeePerGas = UInt<384>::zero();
    std::vector<ETHTransaction> Transactions;
    std::vector<UInt<256>> Uncles;
  };

  int64_t ioSearchUncle(CConnection *connection, int64_t height, const std::string &hash, int64_t bestBlockHeight, std::string &publicHash);

  // Raw Ethereum API - methods
  CNetworkClient::EOperationStatus ethGetBalance(CConnection *connection, const std::string &address, UInt<384> *balance);
  CNetworkClient::EOperationStatus ethGasPrice(CConnection *connection, UInt<384> *gasPrice);
  CNetworkClient::EOperationStatus ethMaxPriorityFeePerGas(CConnection *connection, UInt<384> *maxPriorityFeePerGas);
  CNetworkClient::EOperationStatus ethGetTransactionCount(CConnection *connection, const std::string &address, uint64_t *count);
  CNetworkClient::EOperationStatus ethBlockNumber(CConnection *connection, uint64_t *blockNumber);
  CNetworkClient::EOperationStatus ethGetBlockByNumber(CConnection *connection, uint64_t height, ETHBlock &block);
  CNetworkClient::EOperationStatus ethGetUncleByBlockNumberAndIndex(CConnection *connection, uint64_t height, unsigned uncleIndex, ETHBlock &block);
  CNetworkClient::EOperationStatus ethGetTransactionByHash(CConnection *connection, const UInt<256> &txid, ETHTransaction &tx);
  CNetworkClient::EOperationStatus ethGetTransactionReceipt(CConnection *connection, const UInt<256> &txid, ETHTransactionReceipt &receipt);

  CNetworkClient::EOperationStatus ethSignTransactionOld(CConnection *connection,
                                                         const std::string &from,
                                                         const std::string &to,
                                                         const UInt<384> &value,
                                                         uint64_t gas,
                                                         const UInt<384> &gasPrice,
                                                         uint64_t nonce,
                                                         std::string &txData,
                                                         std::string &txId);

  CNetworkClient::EOperationStatus ethSignTransaction1559(CConnection *connection,
                                                          const std::string &from,
                                                          const std::string &to,
                                                          const UInt<384> &value,
                                                          uint64_t gas,
                                                          const UInt<384> &maxPriorityFeePerGas,
                                                          const UInt<384> &maxFeePerGas,
                                                          uint64_t nonce,
                                                          std::string &txData,
                                                          std::string &txId);

  CNetworkClient::EOperationStatus ethSendRawTransaction(CConnection *connection, const std::string &txData);

  CNetworkClient::EOperationStatus personalUnlockAccount(CConnection *connection, const std::string &address, const std::string &passPhrase, unsigned seconds);

private:
  asyncBase *WorkFetcherBase_ = nullptr;
  CCoinInfo CoinInfo_;
  std::string HostName_;
  std::string FullHostName_;
  HostAddress Address_;

  WorkFetcherContext WorkFetcher_;
  std::string MiningAddress_;
  xmstream EthGetWork_;
};
