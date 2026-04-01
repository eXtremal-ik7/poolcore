#pragma once

#include "poolcore/poolCore.h"
#include <atomic>
#include <memory>
#include <vector>

class CBitcoinRpcClient;

class CBitcoinNetworkClient : public CNetworkClient {
public:
  CBitcoinNetworkClient(asyncBase *base, const CCoinInfo &coinInfo, loguru::LogChannel &logChannel);
  ~CBitcoinNetworkClient() override = default;

  void addGetWorkClient(CBitcoinRpcClient *client);
  void addRpcClient(CBitcoinRpcClient *client);

  // CNetworkClient interface
  bool ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockConfirmationsQuery> &query) override;
  bool ioGetBlockExtraInfo(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockExtraInfoQuery> &query) override;
  bool ioGetBalance(asyncBase *base, GetBalanceResult &result) override;
  EOperationStatus ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const UInt<384> &value, BuildTransactionResult &result) override;
  EOperationStatus ioSendTransaction(asyncBase *base, const std::string &txData, const std::string &txId, std::string &error) override;
  EOperationStatus ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, UInt<384> *txFee, std::string &error) override;
  EOperationStatus ioListUnspent(asyncBase *base, ListUnspentResult &result) override;
  EOperationStatus ioZSendMany(asyncBase *base, const std::string &source, const std::string &destination, const UInt<384> &amount, const std::string &memo, uint64_t minConf, const UInt<384> &fee, ZSendMoneyResult &result) override;
  EOperationStatus ioZGetBalance(asyncBase *base, const std::string &address, UInt<384> *balance) override;
  EOperationStatus ioWalletService(asyncBase *base, std::string &error) override;
  void aioSubmitBlock(asyncBase *base, const void *data, size_t size, CSubmitBlockOperation *operation) override;
  void poll() override;

  size_t getWorkClientsNum() const override { return GetWorkClients_.size(); }

protected:
  void updateFeeEstimation() override;
  void doAdvanceWorkFetcher() override;

private:
  std::vector<std::unique_ptr<CBitcoinRpcClient>> GetWorkClients_;
  std::vector<std::unique_ptr<CBitcoinRpcClient>> RpcClients_;
  std::atomic<size_t> CurrentRpcIdx_{0};
  size_t CurrentWorkFetcherIdx_ = 0;
};
