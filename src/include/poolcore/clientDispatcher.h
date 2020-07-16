#pragma once

#include "poolCore.h"
#include "loguru.hpp"
#include <memory>

class CNetworkClientDispatcher {
public:
  CNetworkClientDispatcher() {}
  void addClient(CNetworkClient *client) {
    Clients_.emplace_back(client);
  }

  bool getBalance(int64_t *balance);
  bool getBlockTemplate(std::string &result);
  bool sendMoney(const char *address, int64_t value, std::string &txid);
  void poll();

private:
  std::vector<std::unique_ptr<CNetworkClient>> Clients_;
  size_t CurrentWorkFetcherIdx = 0;
  size_t CurrentClientIdx = 0;
};
