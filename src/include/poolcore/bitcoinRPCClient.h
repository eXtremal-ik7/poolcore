#pragma once

#include "poolcore/poolCore.h"
#include "poolcommon/httpClient.h"
#include "asyncio/asyncio.h"
#include <rapidjson/document.h>
#include <vector>
#include "loguru.hpp"

class xmstream;

class CBitcoinRpcClient : public CNetworkClient {
public:
  CBitcoinRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, const char *login, const char *password, const char *wallet, bool longPollEnabled);

  virtual bool ioGetBalance(asyncBase *base, GetBalanceResult &result) override;
  virtual bool ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockConfirmationsQuery> &query) override;
  virtual EOperationStatus ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const UInt<384> &value, BuildTransactionResult &result) override;
  virtual EOperationStatus ioSendTransaction(asyncBase *base, const std::string &txData, const std::string&, std::string &error) override;
  virtual EOperationStatus ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, UInt<384> *txFee, std::string &error) override;
  virtual void aioSubmitBlock(asyncBase *base, const void *data, size_t size, CSubmitBlockOperation *operation) override;
  virtual EOperationStatus ioListUnspent(asyncBase *base, ListUnspentResult &result) override;
  virtual EOperationStatus ioZSendMany(asyncBase *base, const std::string &source, const std::string &destination, const UInt<384> &amount, const std::string &memo, uint64_t minConf, const UInt<384> &fee, CNetworkClient::ZSendMoneyResult &result) override;
  virtual EOperationStatus ioZGetBalance(asyncBase *base, const std::string &address, UInt<384> *balance) override;
  virtual EOperationStatus ioWalletService(asyncBase *base, std::string &error) override;

  virtual bool ioGetBlockExtraInfo(asyncBase*, int64_t, std::vector<GetBlockExtraInfoQuery>&) override {
    return false;
  }
  virtual bool ioGetBlockTxFees(asyncBase *base, int64_t fromHeight, int64_t toHeight, std::vector<BlockTxFeeInfo> &result) override;
  EFeeEstimationMode feeEstimationMode() const override { return EFeeEstimationMode::BlockTxFees; }
  virtual void poll() override;

private:
  struct GBTInstance {
    std::string LongPollId;
    uint64_t WorkId;
    aioUserEvent *TimerEvent;
  };

  struct RpcQueryResult {
    std::string Error;
    int ErrorCode = 0;
    unsigned HttpStatus = 0;
  };

private:
  EOperationStatus signRawTransaction(asyncBase *base, const std::string &fundedTransaction, std::string &signedTransaction, std::string &error);

  std::string buildPostQuery(const char *data, size_t size);
  std::string buildPostQuery(const std::string &jsonBody);
  std::string buildPostQuery(const xmstream &postData);

  template<rapidjson::ParseFlag flag = rapidjson::kParseDefaultFlags>
  EOperationStatus ioRpcQuery(asyncBase *base, const std::string &request, rapidjson::Document &document,
                              uint64_t timeout, RpcQueryResult &rpcResult)
  {
    HttpResponse response;
    AsyncOpStatus status = RpcEndpoint_.ioRequest(base, request, response, timeout);
    if (status != aosSuccess) {
      CLOG_F(WARNING, "{} {}: error code: {}", CoinInfo_.Name, FullHostName_, static_cast<unsigned>(status));
      return status == aosTimeout ? EStatusTimeout : EStatusNetworkError;
    }

    document.Parse<flag>(response.Body.data(), response.Body.size());
    rpcResult.HttpStatus = response.StatusCode;

    if (response.StatusCode != 200) {
      if (!document.HasParseError()) {
        if (document.HasMember("error") && document["error"].IsObject()) {
          rapidjson::Value &value = document["error"];
          if (value.HasMember("code") && value["code"].IsInt())
            rpcResult.ErrorCode = value["code"].GetInt();
          if (value.HasMember("message") && value["message"].IsString())
            rpcResult.Error = value["message"].GetString();
        }
      }

      CLOG_F(WARNING, "{} {}: http result code: {}, data: {}",
             CoinInfo_.Name,
             FullHostName_,
             response.StatusCode,
             response.Body.empty() ? "<null>" : response.Body.c_str());
      return EStatusProtocolError;
    }

    if (document.HasParseError()) {
      CLOG_F(WARNING, "{} {}: JSON parse error", CoinInfo_.Name, FullHostName_);
      return EStatusProtocolError;
    }

    return EStatusOk;
  }

  template<rapidjson::ParseFlag flag = rapidjson::kParseDefaultFlags>
  EOperationStatus ioRpcQuery(asyncBase *base, const std::string &request, rapidjson::Document &document, uint64_t timeout) {
    RpcQueryResult unused;
    return ioRpcQuery<flag>(base, request, document, timeout, unused);
  }

  void sendWorkFetchRequest();
  void onWorkFetcherResponse(AsyncOpStatus status, HttpResponse response);
  void onWorkFetchTimeout();

private:
  CCoinInfo CoinInfo_;
  std::string FullHostName_;

  CHttpEndpoint RpcEndpoint_;
  CHttpConnection WorkFetcherHttpClient_;
  GBTInstance WorkFetcher_;
  unsigned WorkFetchGeneration_ = 0;
  bool HasLongPoll_;
  bool HasGetWalletInfo_ = true;
  bool HasGetBlockChainInfo_ = true;
  bool HasSignRawTransactionWithWallet_ = true;

  // Cached prepared HTTP requests for static RPC calls
  std::string GetWalletInfoRequest_;
  std::string BalanceRequest_;
  std::string BalanceWithImmaturedRequest_;
  std::string GbtRequestNoLongPoll_; // non-longpoll getblocktemplate (body is constant)
};
