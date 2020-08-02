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
