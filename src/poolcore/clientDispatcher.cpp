#include "poolcore/clientDispatcher.h"

#include "poolcore/blockTemplate.h"
#include "poolcore/thread.h"
#include "loguru.hpp"

bool CNetworkClientDispatcher::ioGetBalance(asyncBase *base, CNetworkClient::GetBalanceResult &result)
{
  unsigned threadId = GetGlobalThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = RPCClients_.size(); i != ie; ++i) {
    if (RPCClients_[currentClientIdx]->ioGetBalance(base, result))
      return true;
    currentClientIdx = (currentClientIdx + 1) % RPCClients_.size();
  }

  return false;
}

bool CNetworkClientDispatcher::ioGetBlockConfirmations(asyncBase *base, int64_t orphanAgeLimit, std::vector<CNetworkClient::GetBlockConfirmationsQuery> &query)
{
  unsigned threadId = GetGlobalThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = RPCClients_.size(); i != ie; ++i) {
    if (RPCClients_[currentClientIdx]->ioGetBlockConfirmations(base, orphanAgeLimit, query))
      return true;
    currentClientIdx = (currentClientIdx + 1) % RPCClients_.size();
  }

  return false;
}

bool CNetworkClientDispatcher::ioGetBlockExtraInfo(asyncBase *base, int64_t orphanAgeLimit, std::vector<CNetworkClient::GetBlockExtraInfoQuery> &query)
{
  unsigned threadId = GetGlobalThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = RPCClients_.size(); i != ie; ++i) {
    if (RPCClients_[currentClientIdx]->ioGetBlockExtraInfo(base, orphanAgeLimit, query))
      return true;
    currentClientIdx = (currentClientIdx + 1) % RPCClients_.size();
  }

  return false;
}

CNetworkClient::EOperationStatus CNetworkClientDispatcher::ioListUnspent(asyncBase *base, CNetworkClient::ListUnspentResult &result)
{
  CNetworkClient::EOperationStatus status = CNetworkClient::EStatusUnknownError;
  unsigned threadId = GetGlobalThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = RPCClients_.size(); i != ie; ++i) {
    status = RPCClients_[currentClientIdx]->ioListUnspent(base, result);
    if (status == CNetworkClient::EStatusOk)
      return CNetworkClient::EStatusOk;
    currentClientIdx = (currentClientIdx + 1) % RPCClients_.size();
  }

  return status;
}

CNetworkClient::EOperationStatus CNetworkClientDispatcher::ioBuildTransaction(asyncBase *base, const std::string &address, const std::string &changeAddress, const int64_t value, CNetworkClient::BuildTransactionResult &result)
{
  CNetworkClient::EOperationStatus status = CNetworkClient::EStatusUnknownError;
  unsigned threadId = GetGlobalThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = RPCClients_.size(); i != ie; ++i) {
    status = RPCClients_[currentClientIdx]->ioBuildTransaction(base, address, changeAddress, value, result);
    if (status == CNetworkClient::EStatusOk)
      return CNetworkClient::EStatusOk;
    currentClientIdx = (currentClientIdx + 1) % RPCClients_.size();
  }

  return status;
}

CNetworkClient::EOperationStatus CNetworkClientDispatcher::ioSendTransaction(asyncBase *base, const std::string &txData, const std::string &txId, std::string &error)
{
  CNetworkClient::EOperationStatus status = CNetworkClient::EStatusUnknownError;
  unsigned threadId = GetGlobalThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = RPCClients_.size(); i != ie; ++i) {
    status = RPCClients_[currentClientIdx]->ioSendTransaction(base, txData, txId, error);
    if (status == CNetworkClient::EStatusOk)
      return CNetworkClient::EStatusOk;
    currentClientIdx = (currentClientIdx + 1) % RPCClients_.size();
  }

  return status;
}

CNetworkClient::EOperationStatus CNetworkClientDispatcher::ioWalletService(asyncBase *base, std::string &error)
{
  CNetworkClient::EOperationStatus status = CNetworkClient::EStatusUnknownError;
  unsigned threadId = GetGlobalThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = RPCClients_.size(); i != ie; ++i) {
    status = RPCClients_[currentClientIdx]->ioWalletService(base, error);
    if (status == CNetworkClient::EStatusOk)
      return CNetworkClient::EStatusOk;
    currentClientIdx = (currentClientIdx + 1) % RPCClients_.size();
  }

  return status;
}

CNetworkClient::EOperationStatus CNetworkClientDispatcher::ioGetTxConfirmations(asyncBase *base, const std::string &txId, int64_t *confirmations, int64_t *txFee, std::string &error)
{
  CNetworkClient::EOperationStatus status = CNetworkClient::EStatusUnknownError;
  unsigned threadId = GetGlobalThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = RPCClients_.size(); i != ie; ++i) {
    status = RPCClients_[currentClientIdx]->ioGetTxConfirmations(base, txId, confirmations, txFee, error);
    if (status == CNetworkClient::EStatusOk)
      return CNetworkClient::EStatusOk;
    currentClientIdx = (currentClientIdx + 1) % RPCClients_.size();
  }

  return status;
}

void CNetworkClientDispatcher::aioSubmitBlock(asyncBase *base, const void *data, size_t size, CNetworkClient::SumbitBlockCb callback)
{
  CNetworkClient::CSubmitBlockOperation *submitOperation = new CNetworkClient::CSubmitBlockOperation(callback, GetWorkClients_.size());
  for (size_t i = 0, ie = GetWorkClients_.size(); i != ie; ++i)
    GetWorkClients_[i]->aioSubmitBlock(base, GetWorkClients_[i]->prepareBlock(data, size), submitOperation);
}

// ZEC specific
CNetworkClient::EOperationStatus CNetworkClientDispatcher::ioZGetBalance(asyncBase *base, const std::string &address, int64_t *result)
{
  CNetworkClient::EOperationStatus status = CNetworkClient::EStatusUnknownError;
  unsigned threadId = GetGlobalThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = RPCClients_.size(); i != ie; ++i) {
    status = RPCClients_[currentClientIdx]->ioZGetBalance(base, address, result);
    if (status == CNetworkClient::EStatusOk)
      return CNetworkClient::EStatusOk;
    currentClientIdx = (currentClientIdx + 1) % RPCClients_.size();
  }

  return status;
}

CNetworkClient::EOperationStatus CNetworkClientDispatcher::ioZSendMoney(asyncBase *base, const std::string &source, const std::string &destination, int64_t amount, const std::string &memo, uint64_t minConf, int64_t fee, CNetworkClient::ZSendMoneyResult &result)
{
  CNetworkClient::EOperationStatus status = CNetworkClient::EStatusUnknownError;
  unsigned threadId = GetGlobalThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = RPCClients_.size(); i != ie; ++i) {
    status = RPCClients_[currentClientIdx]->ioZSendMany(base, source, destination, amount, memo, minConf, fee, result);
    if (status == CNetworkClient::EStatusOk)
      return CNetworkClient::EStatusOk;
    currentClientIdx = (currentClientIdx + 1) % RPCClients_.size();
  }

  return status;
}

void CNetworkClientDispatcher::poll()
{
  if (!GetWorkClients_.empty()) {
    GetWorkClients_[CurrentWorkFetcherIdx]->poll();
  } else {
    LOG_F(ERROR, "%s: no nodes configured", CoinInfo_.Name.c_str());
  }
}

void CNetworkClientDispatcher::onWorkFetchReconnectTimer()
{
  if (WorkState_ == EWorkLost) {
    // Stop all miners if 5 or more seconds no work
    auto now = std::chrono::steady_clock::now();
    int64_t noWorkTime = std::chrono::duration_cast<std::chrono::seconds>(now - ConnectionLostTime_).count();
    if (noWorkTime >= 5) {
      for (auto &instance : LinkedInstances_)
        instance->stopWork();
      WorkState_ = EWorkMinersStopped;
    }
  }

  CurrentWorkFetcherIdx = (CurrentWorkFetcherIdx + 1) % GetWorkClients_.size();
  GetWorkClients_[CurrentWorkFetcherIdx]->poll();
}

void CNetworkClientDispatcher::onWorkFetcherConnectionError()
{
  if (WorkState_ == EWorkOk) {
    WorkState_ = EWorkLost;
    ConnectionLostTime_ = std::chrono::steady_clock::now();
  }

  // 500 milliseconds
  userEventStartTimer(WorkFetcherReconnectTimer_, 500000, 1);
}

void CNetworkClientDispatcher::onWorkFetcherConnectionLost()
{
  if (WorkState_ == EWorkOk) {
    WorkState_ = EWorkLost;
    ConnectionLostTime_ = std::chrono::steady_clock::now();
  }

  // 100 milliseconds
  userEventStartTimer(WorkFetcherReconnectTimer_, 100000, 1);
}

void CNetworkClientDispatcher::onWorkFetcherNewWork(CBlockTemplate *blockTemplate)
{
  WorkState_ = EWorkOk;
  intrusive_ptr<CBlockTemplate> holder(blockTemplate);
  for (auto &instance : LinkedInstances_)
    instance->checkNewBlockTemplate(blockTemplate, Backend_);
}
