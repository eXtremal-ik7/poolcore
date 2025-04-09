#pragma once

#include <string>
#include <vector>
#include "poolcommon/jsonSerializer.h"
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
  std::string StratumVersion;
  int64_t connectPort;
};

struct StratumAuthorize {
  std::string login;
  std::string password;
};

struct StratumSubmit {
  std::string WorkerName;
  std::string JobId;
  struct {
    uint32_t Time;
    uint32_t Nonce;
    std::vector<uint8_t> MutableExtraNonce;
    std::optional<uint32_t> VersionBits;
  } BTC;
  struct {
    uint64_t Nonce;
  } ETH;
  struct {
    uint32_t Time;
    std::string Nonce;
    std::string Solution;
  } ZEC;
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

struct CStratumMessage {
  int64_t IntegerId;
  std::string StringId;
  EStratumMethodTy Method;
  StratumMiningSubscribe Subscribe;
  StratumAuthorize Authorize;
  StratumSubmit Submit;
  StratumMultiVersion MultiVersion;
  StratumMiningConfigure MiningConfigure;
  StratumMiningSuggestDifficulty MiningSuggestDifficulty;
  std::string Error;

  void addId(JSON::Object &object) {
    if (!StringId.empty())
      object.addString("id", StringId);
    else
      object.addInt("id", IntegerId);
  }
};
