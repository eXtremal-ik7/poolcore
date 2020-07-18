#pragma once

#include <tbb/concurrent_queue.h>
#include <string>
#include <thread>
#include <vector>
#include "rapidjson/document.h"

class CNetworkClientDispatcher;

struct CCoinInfo {
  enum EAddressType {
    EP2PKH = 1,
    EPS2H = 2,
    EBech32 = 4,
    EZAddr = 8
  };

  std::string Name;
  int64_t RationalPartSize;
  EAddressType PayoutAddressType;
  bool SegwitEnabled;

  std::vector<uint8_t> PubkeyAddressPrefix;
  std::vector<uint8_t> ScriptAddressPrefix;

  uint16_t DefaultRpcPort;

  bool checkAddress(const std::string &address, EAddressType type);
};

class CNetworkClient {
public:
  ~CNetworkClient() {}

  virtual bool ioGetBalance(int64_t *balance) = 0;
  virtual bool ioGetBlockTemplate(std::string &result) = 0;
  virtual bool ioSendMoney(const char *address, int64_t value, std::string &txid) = 0;

  virtual void poll() = 0;

  void setDispatcher(CNetworkClientDispatcher *dispatcher) { Dispatcher_ = dispatcher; }

protected:
  CNetworkClientDispatcher *Dispatcher_ = nullptr;
};

