#pragma once

#include "poolcore/poolCore.h"
#include "poolcommon/uint.h"
#include "poolcommon/httpClient.h"
#include <rapidjson/document.h>
#include "loguru.hpp"
#include <chrono>

struct PoolBackendConfig;
class xmstream;

class CEthereumRpcClient : public CNetworkClient {
public:
  CEthereumRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, PoolBackendConfig &config);

  virtual bool ioGetBalance(asyncBase *base, GetBalanceResult &result) override;
  virtual bool ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockConfirmationsQuery> &queries) override;
  virtual bool ioGetBlockExtraInfo(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockExtraInfoQuery> &queries) override;
  virtual EOperationStatus ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const UInt<384> &value, BuildTransactionResult &result) override;
  virtual EOperationStatus ioSendTransaction(asyncBase *base, const std::string &txData, const std::string &txId, std::string &error) override;
  virtual EOperationStatus ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, UInt<384> *txFee, std::string &error) override;
  virtual EOperationStatus ioWalletService(asyncBase *base, std::string &error) override;
  virtual void aioSubmitBlock(asyncBase *base, const void *data, size_t size, CSubmitBlockOperation *operation) override;

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
  EFeeEstimationMode feeEstimationMode() const override { return EFeeEstimationMode::Unsupported; }

  virtual void poll() override;

private:
  struct SubmitBlockContext {
    CEthereumRpcClient *Client;
    CSubmitBlockOperation *Operation;
  };

  struct WorkFetcherContext {
    std::chrono::time_point<std::chrono::steady_clock> LastTemplateTime;
    aioUserEvent *TimerEvent;
    uint64_t WorkId;
    uint64_t Height;
  };

  struct RpcQueryResult {
    std::string Error;
    int ErrorCode = 0;
  };

private:
  std::string buildPostQuery(const char *data, size_t size);
  std::string buildPostQuery(const std::string &jsonBody);
  std::string buildPostQuery(const xmstream &postData);

  template<rapidjson::ParseFlag flag = rapidjson::kParseDefaultFlags>
  EOperationStatus ioRpcQuery(asyncBase *base, const std::string &request, rapidjson::Document &document, uint64_t timeout)
  {
    HttpResponse response;
    AsyncOpStatus status = RpcEndpoint_.ioRequest(base, request, response, timeout);
    if (status != aosSuccess) {
      CLOG_F(WARNING, "{} {}: error code: {}", CoinInfo_.Name, FullHostName_, static_cast<unsigned>(status));
      return status == aosTimeout ? EStatusTimeout : EStatusNetworkError;
    }

    document.Parse<flag>(response.Body.data(), response.Body.size());

    if (response.StatusCode != 200) {
      CLOG_F(WARNING, "{} {}: request error code: {} (http result code: {}, data: {})",
             CoinInfo_.Name,
             FullHostName_,
             static_cast<unsigned>(status),
             response.StatusCode,
             response.Body.empty() ? "<null>" : response.Body.c_str());
      return EStatusUnknownError;
    }

    if (document.HasParseError()) {
      CLOG_F(WARNING, "{} {}: JSON parse error", CoinInfo_.Name, FullHostName_);
      return EStatusProtocolError;
    }

    if (document.HasMember("error") && document["error"].IsObject()) {
      rapidjson::Value &value = document["error"];
      if (value.HasMember("code") && value["code"].IsInt())
        LastRpcErrorCode_ = value["code"].GetInt();
      if (value.HasMember("message") && value["message"].IsString())
        LastRpcError_ = value["message"].GetString();

      CLOG_F(WARNING, "{} {}: Error code: {}, Error message: {}",
             CoinInfo_.Name,
             FullHostName_,
             LastRpcErrorCode_,
             LastRpcError_);

      return EStatusProtocolError;
    }

    return EStatusOk;
  }

  void onWorkFetcherResponse(AsyncOpStatus status, HttpResponse response);
  void onWorkFetchTimeout();

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

  int64_t ioSearchUncle(asyncBase *base, int64_t height, const std::string &hash, int64_t bestBlockHeight, std::string &publicHash);

  // Raw Ethereum API - methods
  CNetworkClient::EOperationStatus ethGetBalance(asyncBase *base, const std::string &address, UInt<384> *balance);
  CNetworkClient::EOperationStatus ethGasPrice(asyncBase *base, UInt<384> *gasPrice);
  CNetworkClient::EOperationStatus ethMaxPriorityFeePerGas(asyncBase *base, UInt<384> *maxPriorityFeePerGas);
  CNetworkClient::EOperationStatus ethGetTransactionCount(asyncBase *base, const std::string &address, uint64_t *count);
  CNetworkClient::EOperationStatus ethBlockNumber(asyncBase *base, uint64_t *blockNumber);
  CNetworkClient::EOperationStatus ethGetBlockByNumber(asyncBase *base, uint64_t height, ETHBlock &block);
  CNetworkClient::EOperationStatus ethGetUncleByBlockNumberAndIndex(asyncBase *base, uint64_t height, unsigned uncleIndex, ETHBlock &block);
  CNetworkClient::EOperationStatus ethGetTransactionByHash(asyncBase *base, const UInt<256> &txid, ETHTransaction &tx);
  CNetworkClient::EOperationStatus ethGetTransactionReceipt(asyncBase *base, const UInt<256> &txid, ETHTransactionReceipt &receipt);

  CNetworkClient::EOperationStatus ethSignTransactionOld(asyncBase *base,
                                                         const std::string &from,
                                                         const std::string &to,
                                                         const UInt<384> &value,
                                                         uint64_t gas,
                                                         const UInt<384> &gasPrice,
                                                         uint64_t nonce,
                                                         std::string &txData,
                                                         std::string &txId);

  CNetworkClient::EOperationStatus ethSignTransaction1559(asyncBase *base,
                                                          const std::string &from,
                                                          const std::string &to,
                                                          const UInt<384> &value,
                                                          uint64_t gas,
                                                          const UInt<384> &maxPriorityFeePerGas,
                                                          const UInt<384> &maxFeePerGas,
                                                          uint64_t nonce,
                                                          std::string &txData,
                                                          std::string &txId);

  CNetworkClient::EOperationStatus ethSendRawTransaction(asyncBase *base, const std::string &txData);

  CNetworkClient::EOperationStatus personalUnlockAccount(asyncBase *base, const std::string &address, const std::string &passPhrase, unsigned seconds);

private:
  CCoinInfo CoinInfo_;
  std::string FullHostName_;

  CHttpEndpoint RpcEndpoint_;
  CHttpConnection WorkFetcherClient_;
  WorkFetcherContext WorkFetcher_;
  std::string MiningAddress_;
  std::string EthGetWorkRequest_;

  std::string LastRpcError_;
  int LastRpcErrorCode_ = 0;
};
