#pragma once

#include <string>
#include <vector>
#include "poolcommon/utils.h"
#include "p2putils/strExtras.h"
#include <optional>

enum EStratumMethodTy {
  ESubscribe = 0,
  EAuthorize,
  EExtraNonceSubscribe,
  ESubmit,
  EMultiVersion,
  EMiningConfigure,
  EMiningSuggestDifficulty,
  ESubmitHashrate,
  ELast
};

enum EStratumDecodeStatusTy {
  EStratumStatusOk = 0,
  EStratumStatusJsonError,
  EStratumStatusFormatError
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

struct StratumMiningSuggestDifficulty {
  double Difficulty;
};
