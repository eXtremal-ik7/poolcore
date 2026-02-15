#pragma once

#include <string>
#include <vector>
#include "poolcore/backendData.h"
#include "rapidjson/document.h"

enum EErrorType {
  EOk = 0,
  ENotExists,
  ETypeMismatch
};

struct CInstanceConfig {
  std::string Name;
  std::string Type;
  std::string Protocol;
  std::vector<std::string> Backends;
  unsigned Port;
  double StratumShareDiff;

  rapidjson::Value InstanceConfig;

  void load(rapidjson::Document &document, const rapidjson::Value &value, std::string &errorPlace, EErrorType *error);
};

void loadNodeConfig(CNodeConfig &config, const rapidjson::Value &value, const std::string &path, std::string &errorDescription, EErrorType *error);

struct CMiningAddressConfig {
  std::string Address;
  std::string PrivateKey;
  uint32_t Weight;
};

struct CCoinConfig {
  std::string Name;
  std::vector<CNodeConfig> GetWorkNodes;
  std::vector<CNodeConfig> RPCNodes;
  unsigned RequiredConfirmations;
  std::string DefaultPayoutThreshold;
  std::string MinimalAllowedPayout;
  unsigned KeepRoundTime;
  unsigned KeepStatsTime;
  unsigned ConfirmationsCheckInterval;
  unsigned PayoutInterval;
  unsigned BalanceCheckInterval;
  unsigned StatisticCheckInterval;
  unsigned ShareTarget;
  unsigned StratumWorkLifeTime;
  std::vector<CMiningAddressConfig> MiningAddresses;
  std::string CoinbaseMsg;
  double ProfitSwitchCoeff;
  std::string PoolZAddr;
  std::string PoolTAddr;

  void load(const rapidjson::Value &value, std::string &errorDescription, EErrorType *error);
};

struct CPoolFrontendConfig {
  bool IsMaster;
  unsigned HttpPort;
  unsigned WorkerThreadsNum;
  unsigned HttpThreadsNum;
  std::string AdminPasswordHash;
  std::string ObserverPasswordHash;
  std::string DbPath;
  std::string PoolName;
  std::string PoolHostProtocol;
  std::string PoolHostAddress;
  std::string PoolActivateLinkPrefix;
  std::string PoolChangePasswordLinkPrefix;
  std::string PoolActivate2faLinkPrefix;
  std::string PoolDeactivate2faLinkPrefix;

  bool SmtpEnabled;
  std::string SmtpServer;
  std::string SmtpLogin;
  std::string SmtpPassword;
  std::string SmtpSenderAddress;
  bool SmtpUseSmtps;
  bool SmtpUseStartTls;

  std::vector<CCoinConfig> Coins;
  std::vector<CInstanceConfig> Instances;

  bool load(rapidjson::Document &document, std::string &errorDescription);
};
