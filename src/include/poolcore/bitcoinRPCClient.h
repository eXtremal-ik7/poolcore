#pragma once

#include "poolcore/poolCore.h"
#include "asyncio/asyncio.h"
#include "asyncio/http.h"
#include <vector>

class CBitcoinRpcClient : public CNetworkClient {
private:
  struct Instance {
    HostAddress Address;
    std::string HostName;
    std::string BasicAuth;
    HTTPClient *WorkFetcher = nullptr;
    HTTPClient *Client = nullptr;
    std::string LongPollId;
  };

private:
  std::vector<Instance> Instances_;
  size_t CurrentInstance_;

public:
  CBitcoinRpcClient(const std::vector<std::string> &nodes, bool segwitEnabled);

  virtual bool getBalance(int64_t *balance) override;
  virtual bool getBlockTemplate(std::string &reslt) override;
  virtual bool sendMoney(const char *address, int64_t value, std::string &txid) override;
};
