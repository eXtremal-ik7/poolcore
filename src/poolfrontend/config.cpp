#include "config.h"
#include <optional>

static inline void setErrorDescription(EErrorType error, EErrorType *errorAcc, const std::string &place, const char *name, const char *requiredType, std::string &errorDescription)
{
  *errorAcc = error;
  if (error == ENotExists) {
    errorDescription = (std::string)"Required parameter '" + name + "' does not exists at " + place;
  } else if (error == ETypeMismatch) {
    errorDescription = (std::string)"Type mismatch: parameter '" + place + " -> " + name + "' must be a " + requiredType;
  }
}

static inline void jsonParseString(const rapidjson::Value &value, const char *name, std::string &out, EErrorType *validAcc, const std::string &place, std::string &errorDescription) {
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsString())
      out = value[name].GetString();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "string", errorDescription);
  } else {
    setErrorDescription(ENotExists, validAcc, place, name, "string", errorDescription);
  }
}

static inline void jsonParseString(const rapidjson::Value &value, const char *name, std::string &out, const std::string &defaultValue, EErrorType *validAcc, const std::string &place, std::string &errorDescription) {
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsString())
      out = value[name].GetString();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "string", errorDescription);
  } else {
    out = defaultValue;
  }
}

static inline void jsonParseBoolean(const rapidjson::Value &value, const char *name, bool *out, EErrorType *validAcc, const std::string &place, std::string &errorDescription) {
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsBool())
      *out = value[name].GetBool();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "boolean", errorDescription);
  } else {
    setErrorDescription(ENotExists, validAcc, place, name, "boolean", errorDescription);
  }
}

static inline void jsonParseBoolean(const rapidjson::Value &value, const char *name, bool *out, bool defaultValue, EErrorType *validAcc, const std::string &place, std::string &errorDescription) {
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsBool())
      *out = value[name].GetBool();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "boolean", errorDescription);
  } else {
    *out = defaultValue;
  }
}

static inline void jsonParseUInt(const rapidjson::Value &value, const char *name, unsigned *out, EErrorType *validAcc, const std::string &place, std::string &errorDescription) {
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsUint())
      *out = value[name].GetUint();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "unsigned integer", errorDescription);
  } else {
    setErrorDescription(ENotExists, validAcc, place, name, "unsigned integer", errorDescription);
  }
}

static inline void jsonParseUInt(const rapidjson::Value &value, const char *name, unsigned *out, unsigned defaultValue, EErrorType *validAcc, const std::string &place, std::string &errorDescription) {
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsUint())
      *out = value[name].GetUint();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "unsigned integer", errorDescription);
  } else {
    *out = defaultValue;
  }
}

static inline void jsonParseDouble(const rapidjson::Value &value, const char *name, double *out, EErrorType *validAcc, const std::string &place, std::string &errorDescription) {
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsFloat())
      *out = value[name].GetDouble();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "floating point number (like 1.0)", errorDescription);
  } else {
    setErrorDescription(ENotExists, validAcc, place, name, "floating point number (like 1.0)", errorDescription);
  }
}

static inline void jsonParseDouble(const rapidjson::Value &value, const char *name, double *out, double defaultValue, EErrorType *validAcc, const std::string &place, std::string &errorDescription) {
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsFloat())
      *out = value[name].GetDouble();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "floating point number (like 1.0)", errorDescription);
  } else {
    *out = defaultValue;
  }
}

static inline void jsonParseStringArray(const rapidjson::Value &value, const char *name, std::vector<std::string> &out, EErrorType *validAcc, const std::string &place, std::string &errorDescription) {
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsArray()) {
      rapidjson::Value::ConstArray array = value[name].GetArray();
      for (rapidjson::SizeType i = 0; i < array.Size(); i++) {
        if (!array[i].IsString()) {
          setErrorDescription(ETypeMismatch, validAcc, place, name, "array of string", errorDescription);
          break;
        }

        out.emplace_back(array[i].GetString());
      }
    } else {
      setErrorDescription(ETypeMismatch, validAcc, place, name, "array of string", errorDescription);
    }
  } else {
    setErrorDescription(ENotExists, validAcc, place, name, "array of string", errorDescription);
  }
}


void CInstanceConfig::load(rapidjson::Document &document, const rapidjson::Value &value, std::string &errorDescription, EErrorType *error)
{
  InstanceConfig = rapidjson::Value(value, document.GetAllocator());

  jsonParseString(value, "name", Name, error, "instances", errorDescription);

  std::string localPath = (std::string)"instances" + " -> " + Name;
  jsonParseString(value, "type", Type, error, localPath, errorDescription);
  jsonParseString(value, "protocol", Protocol, error, localPath, errorDescription);
  jsonParseStringArray(value, "backends", Backends, error, localPath, errorDescription);
  jsonParseUInt(value, "port", &Port, 0, error, localPath, errorDescription);

  if (Protocol == "stratum") {
    if (value.HasMember("shareDiff")) {
      if (value["shareDiff"].IsUint64()) {
        StratumShareDiff = static_cast<double>(value["shareDiff"].GetUint64());
      } else if (value["shareDiff"].IsDouble()) {
        StratumShareDiff = value["shareDiff"].GetDouble();
      } else {
        StratumShareDiff = 0.0;
      }
    }
  }
}


void loadNodeConfig(CNodeConfig &config, const rapidjson::Value &value, const std::string &path, std::string &errorDescription, EErrorType *error)
{
  std::string localPath = path + " -> nodes";
  jsonParseString(value, "type", config.Type, error, localPath, errorDescription);
  jsonParseString(value, "address", config.Address, error, localPath, errorDescription);
  jsonParseString(value, "login", config.Login, "", error, localPath, errorDescription);
  jsonParseString(value, "password", config.Password, "", error, localPath, errorDescription);
  jsonParseString(value, "wallet", config.Wallet, "", error, localPath, errorDescription);
  jsonParseBoolean(value, "longPollEnabled", &config.LongPollEnabled, true, error, localPath, errorDescription);
}

void CCoinConfig::load(const rapidjson::Value &value, std::string &errorDescription, EErrorType *error)
{
  jsonParseString(value, "name", Name, error, "coins", errorDescription);

  // Parse nodes
  std::string localPath = (std::string)"coins" + " -> " + Name;
  if (!value.HasMember("getWorkNodes")) {
    setErrorDescription(ENotExists, error, localPath, "getWorkNodes", "array of objects", errorDescription);
    return;
  }
  if (!value["getWorkNodes"].IsArray()) {
    setErrorDescription(ETypeMismatch, error, localPath, "getWorkNodes", "array of objects", errorDescription);
    return;
  }
  if (!value.HasMember("RPCNodes")) {
    setErrorDescription(ENotExists, error, localPath, "RPCNodes", "array of objects", errorDescription);
    return;
  }
  if (!value["RPCNodes"].IsArray()) {
    setErrorDescription(ETypeMismatch, error, localPath, "RPCNodes", "array of objects", errorDescription);
    return;
  }

  {
    auto array = value["getWorkNodes"].GetArray();
    GetWorkNodes.resize(array.Size());
    for (rapidjson::SizeType i = 0, ie = array.Size(); i != ie; ++i)
      loadNodeConfig(GetWorkNodes[i], array[i], localPath, errorDescription, error);
  }

  {
    auto array = value["RPCNodes"].GetArray();
    RPCNodes.resize(array.Size());
    for (rapidjson::SizeType i = 0, ie = array.Size(); i != ie; ++i)
      loadNodeConfig(RPCNodes[i], array[i], localPath, errorDescription, error);
  }

  jsonParseUInt(value, "requiredConfirmations", &RequiredConfirmations, error, localPath, errorDescription);
  jsonParseString(value, "defaultPayoutThreshold", DefaultPayoutThreshold, error, localPath, errorDescription);
  jsonParseString(value, "minimalAllowedPayout", MinimalAllowedPayout, error, localPath, errorDescription);
  jsonParseUInt(value, "keepRoundTime", &KeepRoundTime, error, localPath, errorDescription);
  jsonParseUInt(value, "keepStatsTime", &KeepStatsTime, error, localPath, errorDescription);
  jsonParseUInt(value, "confirmationsCheckInterval", &ConfirmationsCheckInterval, error, localPath, errorDescription);
  jsonParseUInt(value, "payoutInterval", &PayoutInterval, error, localPath, errorDescription);
  jsonParseUInt(value, "balanceCheckInterval", &BalanceCheckInterval, error, localPath, errorDescription);
  jsonParseUInt(value, "statisticCheckInterval", &StatisticCheckInterval, error, localPath, errorDescription);
  jsonParseUInt(value, "shareTarget", &ShareTarget, error, localPath, errorDescription);
  jsonParseUInt(value, "stratumWorkLifeTime", &StratumWorkLifeTime, 0, error, localPath, errorDescription); 
  if (!value.HasMember("miningAddresses") || !value["miningAddresses"].IsArray()) {
    setErrorDescription(ETypeMismatch, error, localPath, "miningAddress", "array of objects", errorDescription);
    return;
  }

  {
    auto array = value["miningAddresses"].GetArray();
    MiningAddresses.resize(array.Size());
    for (rapidjson::SizeType i = 0, ie = array.Size(); i != ie; ++i) {
      jsonParseString(array[i], "address", MiningAddresses[i].Address, error, localPath + " -> miningAddresses", errorDescription);
      jsonParseString(array[i], "privateKey", MiningAddresses[i].PrivateKey, "", error, localPath + " -> privateKey", errorDescription);
      jsonParseUInt(array[i], "weight", &MiningAddresses[i].Weight, error, localPath + " -> miningAddresses", errorDescription);
    }
  }

  jsonParseString(value, "coinbaseMsg", CoinbaseMsg, "", error, localPath, errorDescription);
  jsonParseDouble(value, "profitSwitchCoeff", &ProfitSwitchCoeff, 0.0, error, localPath, errorDescription);

  // ZEC specific
  jsonParseString(value, "pool_zaddr", PoolZAddr, "", error, localPath, errorDescription);
  jsonParseString(value, "pool_taddr", PoolTAddr, "", error, localPath, errorDescription);
}

bool CPoolFrontendConfig::load(rapidjson::Document &document, std::string &errorDescription)
{
  EErrorType error = EOk;

  // Frontend (object)
  if (!document.HasMember("poolfrontend")) {
    setErrorDescription(ENotExists, &error, ".", "poolfrontend", "array of objects", errorDescription);
    return false;
  }
  if (!document["poolfrontend"].IsObject()) {
    setErrorDescription(ETypeMismatch, &error, ".", "poolfrontend", "array of objects", errorDescription);
    return false;
  }

  {
    const char *localPath = "poolfrontend";
    rapidjson::Value &object = document["poolfrontend"];
    jsonParseBoolean(object, "isMaster", &IsMaster, &error, localPath, errorDescription);
    jsonParseUInt(object, "httpPort", &HttpPort, &error, localPath, errorDescription);
    jsonParseUInt(object, "workerThreadsNum", &WorkerThreadsNum, 0, &error, localPath, errorDescription);
    jsonParseUInt(object, "httpThreadsNum", &HttpThreadsNum, 0, &error, localPath, errorDescription);
    jsonParseString(object, "adminPasswordHash", AdminPasswordHash, "", &error, localPath, errorDescription);
    jsonParseString(object, "observerPasswordHash", ObserverPasswordHash, "", &error, localPath, errorDescription);
    jsonParseString(object, "dbPath", DbPath, &error, localPath, errorDescription);
    jsonParseString(object, "poolName", PoolName, &error, localPath, errorDescription);
    jsonParseString(object, "poolHostProtocol", PoolHostProtocol, &error, localPath, errorDescription);
    jsonParseString(object, "poolHostAddress", PoolHostAddress, &error, localPath, errorDescription);
    jsonParseString(object, "poolActivateLinkPrefix", PoolActivateLinkPrefix, &error, localPath, errorDescription);
    jsonParseString(object, "poolChangePasswordLinkPrefix", PoolChangePasswordLinkPrefix, &error, localPath, errorDescription);
    jsonParseString(object, "poolActivate2faLinkPrefix", PoolActivate2faLinkPrefix, &error, localPath, errorDescription);
    jsonParseString(object, "poolDeactivate2faLinkPrefix", PoolDeactivate2faLinkPrefix, &error, localPath, errorDescription);
    jsonParseBoolean(object, "smtpEnabled", &SmtpEnabled, &error, localPath, errorDescription);
    jsonParseString(object, "smtpServer", SmtpServer, &error, localPath, errorDescription);
    jsonParseString(object, "smtpLogin", SmtpLogin, &error, localPath, errorDescription);
    jsonParseString(object, "smtpPassword", SmtpPassword, &error, localPath, errorDescription);
    jsonParseString(object, "smtpSenderAddress", SmtpSenderAddress, &error, localPath, errorDescription);
    jsonParseBoolean(object, "smtpUseSmtps", &SmtpUseSmtps, false, &error, localPath, errorDescription);
    jsonParseBoolean(object, "smtpUseStartTLS", &SmtpUseStartTls, true, &error, localPath, errorDescription);
  }

  // coins
  if (!document.HasMember("coins")) {
    setErrorDescription(ENotExists, &error, ".", "coins", "array of objects", errorDescription);
    return false;
  }
  if (!document["coins"].IsArray()) {
    setErrorDescription(ETypeMismatch, &error, ".", "coins", "array of objects", errorDescription);
    return false;
  }
  {
    auto array = document["coins"].GetArray();
    Coins.resize(array.Size());
    for (rapidjson::SizeType i = 0, ie = array.Size(); i != ie; ++i)
      Coins[i].load(array[i], errorDescription, &error);
  }

  // instances
  if (!document.HasMember("instances")) {
    setErrorDescription(ENotExists, &error, ".", "instances", "array of objects", errorDescription);
    return false;
  }
  if (!document["instances"].IsArray()) {
    setErrorDescription(ETypeMismatch, &error, ".", "instances", "array of objects", errorDescription);
    return false;
  }
  {
    auto array = document["instances"].GetArray();
    Instances.resize(array.Size());
    for (rapidjson::SizeType i = 0, ie = array.Size(); i != ie; ++i)
      Instances[i].load(document, array[i], errorDescription, &error);
  }

  return error == EOk;
}
