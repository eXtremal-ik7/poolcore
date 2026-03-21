#pragma once

#include "poolCore.h"
#include "poolcommon/httpClient.h"
#include "asyncio/asyncio.h"
#include <chrono>

class CPriceFetcher {
public:
  CPriceFetcher(asyncBase *monitorBase, std::vector<CCoinInfo> &coinInfo, const std::string &coinGeckoApiKey = "");
  double getBtcUsd() const;
  double getPrice(const std::string &coinName) const;
  double getPrice(size_t globalBackendIdx) const;

private:
  void updatePrice();
  void processResponse(const struct CCoinGeckoPrices &response);
  void resetPricesIfStale();

private:
  CHttpClient<> Client_;
  aioUserEvent *TimerEvent_ = nullptr;
  std::vector<CCoinInfo> CoinInfo_;
  std::unordered_map<std::string, size_t> CoinIndexMap_;
  std::string QueryPath_;
  std::atomic<double> BTCPrice_;
  std::unique_ptr<std::atomic<double>[]> CurrentPrices_;
  uint64_t PollInterval_;

  static constexpr std::chrono::minutes StaleTimeout_{30};
  std::chrono::steady_clock::time_point LastSuccessTime_;
};
