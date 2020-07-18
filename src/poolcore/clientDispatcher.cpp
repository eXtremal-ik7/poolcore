#include "poolcore/clientDispatcher.h"
#include "poolcore/thread.h"
#include "loguru.hpp"

bool CNetworkClientDispatcher::ioGetBalance(CNetworkClient::GetBalanceResult &result)
{
  unsigned threadId = GetWorkerThreadId();
  size_t &currentClientIdx = CurrentClientIdx_[threadId];
  for (size_t i = 0, ie = Clients_.size(); i != ie; ++i) {
    if (Clients_[currentClientIdx]->ioGetBalance(result))
      return true;
    currentClientIdx = (currentClientIdx + 1) % Clients_.size();
  }

  return false;
}

bool CNetworkClientDispatcher::ioGetBlockConfirmations(const std::vector<std::string> &hashes, std::vector<int64_t> &result)
{
  LOG_F(ERROR, "ioGetBlockConfirmations api not implemented");
  return false;
}

bool CNetworkClientDispatcher::ioListUnspent(CNetworkClient::ListUnspentResult &result)
{
  LOG_F(ERROR, "ioListUnspent api not implemented");
  return false;
}

bool CNetworkClientDispatcher::ioSendMoney(const char *address, int64_t value, CNetworkClient::SendMoneyResult &result)
{
  LOG_F(ERROR, "ioSendMoney api not implemented");
  return false;
}

// ZEC specific
bool CNetworkClientDispatcher::ioZGetBalance(int64_t *result)
{
  LOG_F(ERROR, "ioZGetBalance api not implemented");
  return false;
}

bool CNetworkClientDispatcher::ioZSendMoney(const std::string &source, const std::string &destination, int64_t amount, const std::string &memo, CNetworkClient::ZSendMoneyResult &result)
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

  userEventStartTimer(WorkFetcherReconnectTimer_, 0.5*1000000, 1);
}

void CNetworkClientDispatcher::onWorkFetcherConnectionLost()
{
  if (WorkState_ == EWorkOk) {
    WorkState_ = EWorkLost;
    ConnectionLostTime_ = std::chrono::steady_clock::now();
  }

  userEventStartTimer(WorkFetcherReconnectTimer_, 0.1*1000000, 1);
}

void CNetworkClientDispatcher::onWorkFetcherNewWork(rapidjson::Value &work)
{
  WorkState_ = EWorkOk;
  for (auto &instance : LinkedInstances_)
    instance->checkNewBlockTemplate(work);
}
