#pragma once

#include <string>
#include <vector>
#include "poolcommon/utils.h"
#include "p2putils/strExtras.h"
#include <optional>

enum StratumMethodTy {
  Subscribe = 0,
  Authorize,
  ExtraNonceSubscribe,
  Submit,
  MultiVersion,
  MiningConfigure,
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
  std::optional<uint32_t> VersionBits;
};

struct StratumMultiVersion {
  uint32_t Version;
};

struct StratumMiningConfigure {
  enum EExtension : unsigned {
    EVersionRolling = 1,
    EMinimumDifficulty = 2,
    ESubscribeExtraNonce = 4
  };

  unsigned ExtensionsField;
  std::optional<uint32_t> VersionRollingMask;
  std::optional<uint32_t> VersionRollingMinBitCount;
  std::optional<double> MinimumDifficultyValue;
};

struct StratumMessage {
  int64_t integerId;
  std::string stringId;
  StratumMethodTy method;

  StratumMiningSubscribe subscribe;
  StratumAuthorize authorize;
  StratumSubmit submit;
  StratumMultiVersion multiVersion;
  StratumMiningConfigure miningConfigure;

  std::string error;
};

StratumDecodeStatusTy decodeStratumMessage(const char *in, size_t size, StratumMessage *out);
