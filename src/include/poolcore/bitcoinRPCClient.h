#pragma once

#include "poolcore/poolCore.h"
#include "poolcore/bitcoinRpc.idl.h"
#include "poolcommon/httpClient.h"
#include "asyncio/asyncio.h"
#include <vector>
#include "loguru.hpp"

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

  template<typename T>
  std::string buildPostQuery(const T &request) {
    std::string body;
    request.serialize(body);
    return buildPostQuery(body);
  }

  template<typename T>
  EOperationStatus ioRpcQuery(asyncBase *base, const std::string &request, T &response, uint64_t timeout, RpcQueryResult &rpcResult)
  {
    constexpr bool NeedResolve = requires(T &r, const typename T::Capture &c) {
      T::resolve(r, c, uint32_t{});
    };

    HttpResponse httpResponse;
    AsyncOpStatus status = RpcEndpoint_.ioRequest(base, request, httpResponse, timeout);
    if (status != aosSuccess) {
      CLOG_F(WARNING, "{} {}: error code: {}", CoinInfo_.Name, FullHostName_, static_cast<unsigned>(status));
      return status == aosTimeout ? EStatusTimeout : EStatusNetworkError;
    }

    rpcResult.HttpStatus = httpResponse.StatusCode;
    [[maybe_unused]] typename T::Capture capture;
    bool parsed;
    if constexpr (NeedResolve)
      parsed = response.parse(httpResponse.Body.data(), httpResponse.Body.size(), capture);
    else
      parsed = response.parse(httpResponse.Body.data(), httpResponse.Body.size());

    if (!parsed) {
      CLOG_F(WARNING, "{} {}: JSON parse error", CoinInfo_.Name, FullHostName_);
      return EStatusProtocolError;
    }

    if (httpResponse.StatusCode != 200) {
      if constexpr (requires { response.Error; }) {
        if (response.Error.has_value()) {
          rpcResult.ErrorCode = response.Error->Code;
          rpcResult.Error = response.Error->Message;
        }
      }
      CLOG_F(WARNING, "{} {}: http result code: {}, data: {}",
             CoinInfo_.Name, FullHostName_, httpResponse.StatusCode,
             httpResponse.Body.empty() ? "<null>" : httpResponse.Body.c_str());
      return EStatusProtocolError;
    }

    if constexpr (NeedResolve) {
      if (!T::resolve(response, capture, CoinInfo_.FractionalPartSize))
        return EStatusProtocolError;
    }

    return EStatusOk;
  }

  template<typename T>
  EOperationStatus ioRpcQuery(asyncBase *base, const std::string &request, T &response, uint64_t timeout) {
    RpcQueryResult unused;
    return ioRpcQuery(base, request, response, timeout, unused);
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
  std::string BlockChainInfoRequest_;
  std::string InfoRequest_;
  std::string GbtRequestNoLongPoll_; // non-longpoll getblocktemplate (body is constant)
};
