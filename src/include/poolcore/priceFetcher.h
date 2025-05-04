#pragma once

#include "poolCore.h"
#include "asyncio/asyncio.h"
#include "asyncio/http.h"

class CPriceFetcher {
public:
  CPriceFetcher(asyncBase *monitorBase, std::vector<CCoinInfo> &coinInfo);
  double getBtcUsd();
  double getPrice(const std::string &coinName);
  double getPrice(size_t index);

private:
  void updatePrice();
  void onConnect(AsyncOpStatus status);
  void onRequest(AsyncOpStatus status);
  void processRequest(const char *data, size_t size);

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
};
