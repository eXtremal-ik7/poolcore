#include "poolcore/bitcoinNetworkClient.h"
#include "poolcore/bitcoinRPCClient.h"
#include "poolcommon/utils.h"
#include "loguru.hpp"

CBitcoinNetworkClient::CBitcoinNetworkClient(asyncBase *base, const CCoinInfo &coinInfo, loguru::LogChannel &logChannel)
{
  LogChannel_ = &logChannel;
  start(base, coinInfo);
}

void CBitcoinNetworkClient::addGetWorkClient(CBitcoinRpcClient *client)
{
  GetWorkClients_.emplace_back(client);
  client->onNewWork = [this](CBlockTemplate *bt) { reportNewWork(bt); };
  client->onConnectionLost = [this]() { reportConnectionLost(); };
  client->setLogChannel(LogChannel_);
}

void CBitcoinNetworkClient::addRpcClient(CBitcoinRpcClient *client)
{
  RpcClients_.emplace_back(client);
  client->setLogChannel(LogChannel_);
}

// Round-robin with atomic shared index

bool CBitcoinNetworkClient::ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<GetBlockConfirmationsQuery> &query)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioGetBlockConfirmations(base, orphanAgeLimit, query); });
}

bool CBitcoinNetworkClient::ioGetBlockExtraInfo(asyncBase *, int64_t, std::vector<GetBlockExtraInfoQuery> &)
{
  return false;
}

bool CBitcoinNetworkClient::ioGetBalance(asyncBase *base, GetBalanceResult &result)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioGetBalance(base, result); });
}

CNetworkClient::EOperationStatus CBitcoinNetworkClient::ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const UInt<384> &value, BuildTransactionResult &result)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioBuildTransaction(base, address, changeAddress, value, result); });
}

CNetworkClient::EOperationStatus CBitcoinNetworkClient::ioSendTransaction(asyncBase *base, const std::string &txData, const std::string &txId, std::string &error)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioSendTransaction(base, txData, txId, error); });
}

CNetworkClient::EOperationStatus CBitcoinNetworkClient::ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, UInt<384> *txFee, std::string &error)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioGetTxConfirmations(base, txId, confirmations, txFee, error); });
}

CNetworkClient::EOperationStatus CBitcoinNetworkClient::ioListUnspent(asyncBase *base, ListUnspentResult &result)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioListUnspent(base, result); });
}

CNetworkClient::EOperationStatus CBitcoinNetworkClient::ioZSendMany(asyncBase *base, const std::string &source, const std::string &destination, const UInt<384> &amount, const std::string &memo, uint64_t minConf, const UInt<384> &fee, ZSendMoneyResult &result)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioZSendMany(base, source, destination, amount, memo, minConf, fee, result); });
}

CNetworkClient::EOperationStatus CBitcoinNetworkClient::ioZGetBalance(asyncBase *base, const std::string &address, UInt<384> *balance)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioZGetBalance(base, address, balance); });
}

CNetworkClient::EOperationStatus CBitcoinNetworkClient::ioWalletService(asyncBase *base, std::string &error)
{
  return roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioWalletService(base, error); });
}

void CBitcoinNetworkClient::aioSubmitBlock(asyncBase *base, const void *data, size_t size, CSubmitBlockOperation *operation)
{
  for (size_t i = 0, n = GetWorkClients_.size(); i < n; ++i)
    GetWorkClients_[i]->aioSubmitBlock(base, data, size, operation);
}

void CBitcoinNetworkClient::poll()
{
  if (!GetWorkClients_.empty()) {
    GetWorkClients_[CurrentWorkFetcherIdx_]->poll();
  } else {
    CLOG_F(ERROR, "{}: no nodes configured", CoinInfo_.Name);
  }
}

void CBitcoinNetworkClient::updateFeeEstimation()
{
  int64_t toHeight = PendingHeight_ - 1;
  int64_t fromHeight;
  if (LastBlockHeight_ == 0) {
    fromHeight = std::max<int64_t>(1, toHeight - 299);
  } else {
    fromHeight = LastBlockHeight_ + 1;
  }

  if (fromHeight > toHeight)
    return;

  std::vector<BlockTxFeeInfo> fees;
  if (!roundRobin(RpcClients_, CurrentRpcIdx_, [&](auto &client) { return client.ioGetBlockTxFees(Base_, fromHeight, toHeight, fees); })) {
    CLOG_F(WARNING, "{}: ioGetBlockTxFees failed", CoinInfo_.Name);
    return;
  }

  for (auto &entry : fees)
    Estimator_->addBlock(entry.Time, entry.TotalFee);
  if (!fees.empty())
    Estimator_->trimWindow(fees.back().Time);
  LastBlockHeight_ = toHeight;

  int64_t avgFee = Estimator_->computeNormalizedAvgFee();
  AverageFee_ = avgFee > 0 ? fromRational(static_cast<uint64_t>(avgFee)) : UInt<384>();
  CLOG_F(INFO, "{}: average block tx fee: {} ({} blocks in window)",
         CoinInfo_.Name,
         FormatMoney(AverageFee_, CoinInfo_.FractionalPartSize),
         Estimator_->size());
}

void CBitcoinNetworkClient::doAdvanceWorkFetcher()
{
  CurrentWorkFetcherIdx_ = (CurrentWorkFetcherIdx_ + 1) % GetWorkClients_.size();
  GetWorkClients_[CurrentWorkFetcherIdx_]->poll();
}
