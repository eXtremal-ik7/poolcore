#pragma once

#include "poolCore.h"
#include "asyncio/asyncio.h"
#include "asyncio/http.h"

class CPriceFetcher {
public:
  CPriceFetcher(asyncBase *monitorBase, const CCoinInfo &coinInfo);
  double getPrice() { return CurrentPrice_.load(); }

private:
  void updatePrice();
  void onConnect(AsyncOpStatus status);
  void onRequest(AsyncOpStatus status);
  bool processRequest(const char *data, size_t size);

private:
  asyncBase *MonitorBase_ = nullptr;
  HTTPClient *Client_ = nullptr;
  aioUserEvent *TimerEvent_ = nullptr;
  CCoinInfo CoinInfo_;
  HTTPParseDefaultContext ParseCtx_;
  HostAddress Address_;
  xmstream PreparedQuery_;
  std::atomic<double> CurrentPrice_;
};
