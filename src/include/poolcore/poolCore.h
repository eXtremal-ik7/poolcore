#pragma once

#include <string>
#include <vector>

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

  bool checkAddress(const std::string &address, EAddressType type);
};

class CNetworkClient {
public:
  ~CNetworkClient() {}

  virtual bool getBalance(int64_t *balance);
  virtual bool getBlockTemplate(std::string &result);
  virtual bool sendMoney(const char *address, int64_t value, std::string &txid);
};
