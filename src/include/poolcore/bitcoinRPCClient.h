#pragma once

#include "poolcore/poolCore.h"
#include "asyncio/asyncio.h"
#include "asyncio/http.h"
#include <chrono>
#include <vector>

struct HTTPClient;

class CBitcoinRpcClient : public CNetworkClient {
public:
  CBitcoinRpcClient(asyncBase *base, unsigned threadsNum, const CCoinInfo &coinInfo, const char *address, const char *login, const char *password);

  virtual bool ioGetBalance(int64_t *balance) override;
  virtual bool ioGetBlockTemplate(std::string &reslt) override;
  virtual bool ioSendMoney(const char *address, int64_t value, std::string &txid) override;

  virtual bool poll() override;

private:
  struct Instance {
    HTTPClient *Client;
    HTTPParseDefaultContext ParseCtx;
    std::string LongPollId;
    std::string PreviousBlock;
    std::chrono::time_point<std::chrono::steady_clock> LastTemplateTime;
    aioUserEvent *TimerEvent;
  };

private:
  void onWorkFetcherConnect(AsyncOpStatus status);
  void onWorkFetcherIncomingData(AsyncOpStatus status);
  void onWorkFetchTimeout();
  void onClientRequestTimeout();

private:
  asyncBase *Base_;
  unsigned ThreadsNum_;
  CCoinInfo CoinInfo_;

  HostAddress Address_;
  std::string HostName_;
  std::string BasicAuth_;

  Instance WorkFetcher_;
  std::vector<Instance> Clients_;
};
