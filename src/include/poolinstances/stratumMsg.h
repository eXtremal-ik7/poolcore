#pragma once

#include <string>
#include <vector>
#include "poolcommon/utils.h"
#include "p2putils/strExtras.h"

enum StratumMethodTy {
  Subscribe = 0,
  Authorize,
  ExtraNonceSubscribe,
  Submit,
  MultiVersion,
  Last
};

enum StratumDecodeStatusTy {
  Ok = 0,
  JsonError,
  FormatError
};

struct StratumMiningSubscribe {
  std::string minerUserAgent;
  std::string sessionId;
  std::string connectHost;
  int64_t connectPort;
};

struct StratumAuthorize {
  std::string login;
  std::string password;
};

struct StratumSubmit {
  std::string WorkerName;
  std::string JobId;
  std::vector<uint8_t> MutableExtraNonce;
  uint32_t Time;
  uint32_t Nonce;
};

struct StratumMultiVersion {
  uint32_t Version;
};

struct StratumMessage {
  int64_t integerId;
  std::string stringId;
  StratumMethodTy method;

  StratumMiningSubscribe subscribe;
  StratumAuthorize authorize;
  StratumSubmit submit;
  StratumMultiVersion multiVersion;

  std::string error;
};

StratumDecodeStatusTy decodeStratumMessage(const char *in, size_t size, StratumMessage *out);

template<typename T>
std::string writeHexBE(T value, unsigned sizeInBytes)
{
  std::string result;
  value = xswap(value);
  value >>= 8*(sizeof(T) - sizeInBytes);

  for (unsigned i = 0; i < sizeInBytes; i++) {
    uint8_t byte = value & 0xFF;
    result.push_back(bin2hexLowerCaseDigit(byte >> 4));
    result.push_back(bin2hexLowerCaseDigit(byte & 0xF));
    value >>= 8;
  }

  return result;
}

template<typename T>
T readHexBE(const char *data, unsigned size)
{
  T result = 0;
  for (unsigned i = 0; i < size; i++) {
    result <<= 8;
    result |= (hexDigit2bin(data[i*2]) << 4);
    result |= hexDigit2bin(data[i*2 + 1]);
  }

  return result;
}

template<typename T>
void writeBinBE(T value, unsigned sizeInBytes, void *out)
{
  value = xswap(value);
  value >>= 8*(sizeof(T) - sizeInBytes);
  uint8_t *p = static_cast<uint8_t*>(out);
  for (size_t i = 0; i < sizeInBytes; i++) {
    p[i] = value & 0xFF;
    value >>= 8;
  }
}
