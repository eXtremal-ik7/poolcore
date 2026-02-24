#pragma once

#include "poolCore.h"
#include "asyncio/asyncio.h"
#include "asyncio/http.h"
#include <chrono>

class CPriceFetcher {
public:
  CPriceFetcher(asyncBase *monitorBase, std::vector<CCoinInfo> &coinInfo, const std::string &coinGeckoApiKey = "");
  double getBtcUsd() const;
  double getPrice(const std::string &coinName) const;
  double getPrice(size_t globalBackendIdx) const;

private:
  void updatePrice();
  void onConnect(AsyncOpStatus status);
  void onRequest(AsyncOpStatus status);
  void processRequest(const char *data, size_t size);
  void resetPricesIfStale();

private:
  asyncBase *MonitorBase_ = nullptr;
  HTTPClient *Client_ = nullptr;
  aioUserEvent *TimerEvent_ = nullptr;
  std::vector<CCoinInfo> CoinInfo_;
  std::unordered_map<std::string, size_t> CoinIndexMap_;
  HTTPParseDefaultContext ParseCtx_;
  HostAddress Address_;
  xmstream PreparedQuery_;
  std::atomic<double> CurrentPrice_;
  std::atomic<double> BTCPrice_;

  std::unique_ptr<std::atomic<double>[]> CurrentPrices_;
  std::string ApiHost_;
  uint64_t PollInterval_;

  static constexpr std::chrono::minutes StaleTimeout_{30};
  std::chrono::steady_clock::time_point LastSuccessTime_;
};
