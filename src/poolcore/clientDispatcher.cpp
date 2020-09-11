#include "poolcore/clientDispatcher.h"
#include "poolcore/thread.h"
#include "loguru.hpp"

bool CNetworkClientDispatcher::ioGetBalance(asyncBase *base, CNetworkClient::GetBalanceResult &result)
{
  unsigned threadId = GetGlobalThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = Clients_.size(); i != ie; ++i) {
    if (Clients_[currentClientIdx]->ioGetBalance(base, result))
      return true;
    currentClientIdx = (currentClientIdx + 1) % Clients_.size();
  }

  return false;
}

bool CNetworkClientDispatcher::ioGetBlockConfirmations(asyncBase *base, std::vector<CNetworkClient::GetBlockConfirmationsQuery> &query)
{
  unsigned threadId = GetGlobalThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = Clients_.size(); i != ie; ++i) {
    if (Clients_[currentClientIdx]->ioGetBlockConfirmations(base, query))
      return true;
    currentClientIdx = (currentClientIdx + 1) % Clients_.size();
  }

  return false;
}

bool CNetworkClientDispatcher::ioListUnspent(asyncBase*, CNetworkClient::ListUnspentResult&)
{
  LOG_F(ERROR, "ioListUnspent api not implemented");
  return false;
}

bool CNetworkClientDispatcher::ioSendMoney(asyncBase *base, const char *address, int64_t value, CNetworkClient::SendMoneyResult &result)
{
  unsigned threadId = GetGlobalThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = Clients_.size(); i != ie; ++i) {
    if (Clients_[currentClientIdx]->ioSendMoney(base, address, value, result))
      return true;
    currentClientIdx = (currentClientIdx + 1) % Clients_.size();
  }

  return false;
}

void CNetworkClientDispatcher::aioSubmitBlock(asyncBase *base, const void *data, size_t size, CNetworkClient::SumbitBlockCb callback)
{
  CNetworkClient::CSubmitBlockOperation *submitOperation = new CNetworkClient::CSubmitBlockOperation(callback, Clients_.size());
  for (size_t i = 0, ie = Clients_.size(); i != ie; ++i)
    Clients_[i]->aioSubmitBlock(base, Clients_[i]->prepareBlock(data, size), submitOperation);
}

// ZEC specific
bool CNetworkClientDispatcher::ioZGetBalance(asyncBase*, int64_t*)
{
  LOG_F(ERROR, "ioZGetBalance api not implemented");
  return false;
}

bool CNetworkClientDispatcher::ioZSendMoney(asyncBase*, const std::string&, const std::string&, int64_t, const std::string&, CNetworkClient::ZSendMoneyResult&)
{
  LOG_F(ERROR, "ioZSendMoney api not implemented");
  return false;
}

void CNetworkClientDispatcher::poll()
{
  if (!Clients_.empty()) {
    Clients_[CurrentWorkFetcherIdx]->poll();
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

  CurrentWorkFetcherIdx = (CurrentWorkFetcherIdx + 1) % Clients_.size();
  Clients_[CurrentWorkFetcherIdx]->poll();
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
