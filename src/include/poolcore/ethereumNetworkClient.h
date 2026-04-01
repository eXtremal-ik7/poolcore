#pragma once

#include "poolcore/poolCore.h"
#include <atomic>
#include <memory>
#include <vector>

class CEthereumRpcClient;

class CEthereumNetworkClient : public CNetworkClient {
public:
  CEthereumNetworkClient(asyncBase *base, const CCoinInfo &coinInfo, loguru::LogChannel &logChannel);
  ~CEthereumNetworkClient() override = default;

  void addGetWorkClient(CEthereumRpcClient *client);
  void addRpcClient(CEthereumRpcClient *client);

  // CNetworkClient interface
  bool ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockConfirmationsQuery> &query) override;
  bool ioGetBlockExtraInfo(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockExtraInfoQuery> &query) override;
  bool ioGetBalance(asyncBase *base, GetBalanceResult &result) override;
  EOperationStatus ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const UInt<384> &value, BuildTransactionResult &result) override;
  EOperationStatus ioSendTransaction(asyncBase *base, const std::string &txData, const std::string &txId, std::string &error) override;
  EOperationStatus ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, UInt<384> *txFee, std::string &error) override;
  EOperationStatus ioListUnspent(asyncBase *, ListUnspentResult &) override { return EStatusUnknownError; }
  EOperationStatus ioZSendMany(asyncBase *, const std::string &, const std::string &, const UInt<384> &, const std::string &, uint64_t, const UInt<384> &, ZSendMoneyResult &) override { return EStatusUnknownError; }
  EOperationStatus ioZGetBalance(asyncBase *, const std::string &, UInt<384> *) override { return EStatusUnknownError; }
  EOperationStatus ioWalletService(asyncBase *base, std::string &error) override;
  void aioSubmitBlock(asyncBase *base, const void *data, size_t size, CSubmitBlockOperation *operation) override;
  void poll() override;

  UInt<384> estimatedBaseReward(int64_t height) const override;
  size_t getWorkClientsNum() const override { return GetWorkClients_.size(); }

protected:
  void doAdvanceWorkFetcher() override;

private:
  std::vector<std::unique_ptr<CEthereumRpcClient>> GetWorkClients_;
  std::vector<std::unique_ptr<CEthereumRpcClient>> RpcClients_;
  std::atomic<size_t> CurrentRpcIdx_{0};
  size_t CurrentWorkFetcherIdx_ = 0;
};
