#include "poolcore/ethereumNetworkClient.h"
#include "poolcore/ethereumRPCClient.h"
#include "poolcore/backend.h"
#include "blockmaker/eth.h"
#include "blockmaker/ethereumBlockTemplate.h"
#include "loguru.hpp"

CEthereumNetworkClient::CEthereumNetworkClient(asyncBase *base, const CCoinInfo &coinInfo, loguru::LogChannel &logChannel)
{
  LogChannel_ = &logChannel;
  start(base, coinInfo);
}

void CEthereumNetworkClient::addGetWorkClient(CEthereumRpcClient *client)
{
  GetWorkClients_.emplace_back(client);
  client->onNewWork = [this](CBlockTemplate *bt) { reportNewWork(bt); };
  client->onConnectionLost = [this]() { reportConnectionLost(); };
  client->onResolveDag = [this](CBlockTemplate *bt, int epochNumber) -> bool {
    auto *ethTemplate = static_cast<CEthereumBlockTemplate*>(bt);
    ethTemplate->DagFile = Backend_->dagFile(epochNumber);
    Backend_->updateDag(epochNumber, CoinInfo_.BigEpoch);
    return ethTemplate->DagFile.get() != nullptr;
  };
  client->setLogChannel(LogChannel_);
}

void CEthereumNetworkClient::addRpcClient(CEthereumRpcClient *client)
{
  RpcClients_.emplace_back(client);
  client->setLogChannel(LogChannel_);
}

// Round-robin

bool CEthereumNetworkClient::ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockConfirmationsQuery> &query)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioGetBlockConfirmations(base, orphanAgeLimit, query); });
}

bool CEthereumNetworkClient::ioGetBlockExtraInfo(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockExtraInfoQuery> &query)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioGetBlockExtraInfo(base, orphanAgeLimit, query); });
}

bool CEthereumNetworkClient::ioGetBalance(asyncBase *base, GetBalanceResult &result)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioGetBalance(base, result); });
}

CNetworkClient::EOperationStatus CEthereumNetworkClient::ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const UInt<384> &value, BuildTransactionResult &result)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioBuildTransaction(base, address, changeAddress, value, result); });
}

CNetworkClient::EOperationStatus CEthereumNetworkClient::ioSendTransaction(asyncBase *base, const std::string &txData, const std::string &txId, std::string &error)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioSendTransaction(base, txData, txId, error); });
}

CNetworkClient::EOperationStatus CEthereumNetworkClient::ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, UInt<384> *txFee, std::string &error)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioGetTxConfirmations(base, txId, confirmations, txFee, error); });
}

CNetworkClient::EOperationStatus CEthereumNetworkClient::ioWalletService(asyncBase *base, std::string &error)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioWalletService(base, error); });
}

void CEthereumNetworkClient::aioSubmitBlock(asyncBase *base, const void *data, size_t size, CSubmitBlockOperation *operation)
{
  for (size_t i = 0, n = GetWorkClients_.size(); i < n; ++i)
    GetWorkClients_[i]->aioSubmitBlock(base, data, size, operation);
}

void CEthereumNetworkClient::poll()
{
  if (!GetWorkClients_.empty()) {
    GetWorkClients_[CurrentWorkFetcherIdx_]->poll();
  } else {
    CLOG_F(ERROR, "{}: no nodes configured", CoinInfo_.Name);
  }
}

void CEthereumNetworkClient::doAdvanceWorkFetcher()
{
  CurrentWorkFetcherIdx_ = (CurrentWorkFetcherIdx_ + 1) % GetWorkClients_.size();
  GetWorkClients_[CurrentWorkFetcherIdx_]->poll();
}

UInt<384> CEthereumNetworkClient::estimatedBaseReward(int64_t height) const
{
  return ETH::getConstBlockReward(CoinInfo_.Name, height);
}
