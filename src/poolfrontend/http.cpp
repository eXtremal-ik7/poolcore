#include "http.h"
#include "poolcommon/utils.h"
#include "poolcore/thread.h"
#include "asyncio/coroutine.h"
#include "asyncio/socket.h"
#include "loguru.hpp"
#include "rapidjson/document.h"
#include "poolcommon/jsonSerializer.h"
#include <cmath>

std::unordered_map<std::string, std::pair<int, PoolHttpConnection::FunctionTy>> PoolHttpConnection::FunctionNameMap_ = {
  // User manager functions
  {"userAction", {hmPost, fnUserAction}},
  {"userCreate", {hmPost, fnUserCreate}},
  {"userResendEmail", {hmPost, fnUserResendEmail}},
  {"userLogin", {hmPost, fnUserLogin}},
  {"userLogout", {hmPost, fnUserLogout}},
  {"userQueryMonitoringSession", {hmPost, fnUserQueryMonitoringSession}},
  {"userChangeEmail", {hmPost, fnUserChangeEmail}},
  {"userChangePasswordForce", {hmPost, fnUserChangePasswordForce}},
  {"userChangePasswordInitiate", {hmPost, fnUserChangePasswordInitiate}},
  {"userGetCredentials", {hmPost, fnUserGetCredentials}},
  {"userGetSettings", {hmPost, fnUserGetSettings}},
  {"userUpdateCredentials", {hmPost, fnUserUpdateCredentials}},
  {"userUpdateSettings", {hmPost, fnUserUpdateSettings}},
  {"userEnumerateAll", {hmPost, fnUserEnumerateAll}},
  {"userEnumerateFeePlan", {hmPost, fnUserEnumerateFeePlan}},
  {"userGetFeePlan", {hmPost, fnUserGetFeePlan}},
  {"userUpdateFeePlan", {hmPost, fnUserUpdateFeePlan}},
  {"userChangeFeePlan", {hmPost, fnUserChangeFeePlan}},
  {"userActivate2faInitiate", {hmPost, fnUserActivate2faInitiate}},
  {"userDeactivate2faInitiate", {hmPost, fnUserDeactivate2faInitiate}},
  // Backend functions
  {"backendManualPayout", {hmPost, fnBackendManualPayout}},
  {"backendQueryCoins", {hmPost, fnBackendQueryCoins}},
  {"backendQueryFoundBlocks", {hmPost, fnBackendQueryFoundBlocks}},
  {"backendQueryPayouts", {hmPost, fnBackendQueryPayouts}},
  {"backendQueryPoolBalance", {hmPost, fnBackendQueryPoolBalance}},
  {"backendQueryPoolStats", {hmPost, fnBackendQueryPoolStats}},
  {"backendQueryPoolStatsHistory", {hmPost, fnBackendQueryPoolStatsHistory}},
  {"backendQueryProfitSwitchCoeff", {hmPost, fnBackendQueryProfitSwitchCoeff}},
  {"backendQueryUserBalance", {hmPost, fnBackendQueryUserBalance}},
  {"backendQueryUserStats", {hmPost, fnBackendQueryUserStats}},
  {"backendQueryUserStatsHistory", {hmPost, fnBackendQueryUserStatsHistory}},
  {"backendQueryWorkerStatsHistory", {hmPost, fnBackendQueryWorkerStatsHistory}},
  {"backendQueryPPLNSPayouts", {hmPost, fnBackendQueryPPLNSPayouts}},
  {"backendQueryPPLNSAcc", {hmPost, fnBackendQueryPPLNSAcc}},
  {"backendUpdateProfitSwitchCoeff", {hmPost, fnBackendUpdateProfitSwitchCoeff}},
  {"backendUpdatePPSConfig", {hmPost, fnBackendUpdatePPSConfig}},
  {"backendPoolLuck", {hmPost, fnBackendPoolLuck}},
  // Instance functions
  {"instanceEnumerateAll", {hmPost, fnInstanceEnumerateAll}},
  // Complex mining stats functions
  {"complexMiningStatsGetInfo", {hmPost, fnComplexMiningStatsGetInfo}}
};

static inline bool rawcmp(Raw data, const char *operand) {
  size_t opSize = strlen(operand);
  return data.size == opSize && memcmp(data.data, operand, opSize) == 0;
}

static inline void jsonParseString(rapidjson::Value &document, const char *name, std::string &out, bool *validAcc) {
  if (document.HasMember(name) && document[name].IsString())
    out = document[name].GetString();
  else
    *validAcc = false;
}

static inline void jsonParseString(rapidjson::Value &document, const char *name, std::string &out, const std::string &defaultValue, bool *validAcc) {
  if (document.HasMember(name)) {
    if (document[name].IsString())
      out = document[name].GetString();
    else
      *validAcc = false;
  } else {
    out = defaultValue;
  }
}

static inline void jsonParseInt64(rapidjson::Value &document, const char *name, int64_t *out, bool *validAcc) {
  if (document.HasMember(name) && document[name].IsInt64())
    *out = document[name].GetInt64();
  else
    *validAcc = false;
}


static inline void jsonParseInt64(rapidjson::Value &document, const char *name, int64_t *out, int64_t defaultValue, bool *validAcc) {
  if (document.HasMember(name)) {
    if (document[name].IsInt64())
      *out = document[name].GetInt64();
    else
      *validAcc = false;
  } else {
    *out = defaultValue;
  }
}


static inline void jsonParseUInt64(rapidjson::Value &document, const char *name, uint64_t *out, int64_t defaultValue, bool *validAcc) {
  if (document.HasMember(name)) {
    if (document[name].IsUint64())
      *out = document[name].GetUint64();
    else
      *validAcc = false;
  } else {
    *out = defaultValue;
  }
}

static inline void jsonParseUInt(rapidjson::Value &document, const char *name, unsigned *out, unsigned defaultValue, bool *validAcc) {
  if (document.HasMember(name)) {
    if (document[name].IsUint())
      *out = document[name].GetUint();
    else
      *validAcc = false;
  } else {
    *out = defaultValue;
  }
}

static inline void jsonParseBoolean(rapidjson::Value &document, const char *name, bool *out, bool *validAcc) {
  if (document.HasMember(name) && document[name].IsBool())
    *out = document[name].GetBool();
  else
    *validAcc = false;
}

static inline void jsonParseBoolean(rapidjson::Value &document, const char *name, bool *out, bool defaultValue, bool *validAcc) {
  if (document.HasMember(name)) {
    if (document[name].IsBool())
      *out = document[name].GetBool();
    else
      *validAcc = false;
  } else {
    *out = defaultValue;
  }
}

static inline void jsonParseNumber(rapidjson::Value &document, const char *name, double *out, bool *validAcc) {
  if (document.HasMember(name)) {
    if (document[name].IsNumber())
      *out = document[name].GetDouble();
    else
      *validAcc = false;
  } else {
    *validAcc = false;
  }
}

static double fnormalize(double v)
{
  if (std::isnan(v) || std::isinf(v))
    return 0.0;
  return v;
}

static inline void parseUserCredentials(rapidjson::Value &document, UserManager::Credentials &credentials, bool *validAcc)
{
  jsonParseString(document, "login", credentials.Login, "", validAcc);
  jsonParseString(document, "password", credentials.Password, "", validAcc);
  jsonParseString(document, "name", credentials.Name, "", validAcc);
  jsonParseString(document, "email", credentials.EMail, "", validAcc);
  jsonParseString(document, "totp", credentials.TwoFactor, "", validAcc);
  jsonParseBoolean(document, "isActive", &credentials.IsActive, false, validAcc);
  jsonParseBoolean(document, "isReadOnly", &credentials.IsReadOnly, false, validAcc);
  jsonParseString(document, "feePlanId", credentials.FeePlan, "", validAcc);
}

static void addUserFeeConfig(xmstream &stream, const UserFeeConfig &config)
{
  JSON::Array cfg(stream);
  for (const auto &pair: config) {
    cfg.addField();
    JSON::Object pairObject(stream);
    pairObject.addString("userId", pair.UserId);
    pairObject.addDouble("percentage", pair.Percentage);
  }
}

static void addUserFeePlan(xmstream &stream, const UserFeePlanRecord &plan)
{
  JSON::Object result(stream);
  result.addString("feePlanId", plan.FeePlanId);
  result.addField("default");
    addUserFeeConfig(stream, plan.Default);
  result.addField("coinSpecificFee");
  {
    JSON::Array coinSpecificFee(stream);
    for (const auto &specificFee: plan.CoinSpecificFee) {
      {
        coinSpecificFee.addField();
        JSON::Object coin(stream);
        coin.addString("coin", specificFee.CoinName);
        coin.addField("config");
          addUserFeeConfig(stream, specificFee.Config);
      }
    }
  }
}

void PoolHttpConnection::run()
{
  aioRead(Socket_, buffer, sizeof(buffer), afNone, 0, readCb, this);
}

int PoolHttpConnection::onParse(HttpRequestComponent *component)
{
  if (component->type == httpRequestDtMethod) {
    Context.method = component->method;
    Context.function = fnUnknown;
    return 1;
  }

  if (component->type == httpRequestDtUriPathElement) {
    // Wait 'api'
    if (Context.function == fnUnknown && rawcmp(component->data, "api")) {
      Context.function = fnApi;
    } else if (Context.function == fnApi) {
      std::string functionName(component->data.data, component->data.data + component->data.size);
      auto It = FunctionNameMap_.find(functionName);
      if (It == FunctionNameMap_.end() || It->second.first != Context.method) {
        reply404();
        return 0;
      }

      Context.function = It->second.second;
      return 1;
    } else {
      reply404();
      return 0;
    }
  } else if (component->type == httpRequestDtData) {
    Context.Request.append(component->data.data, component->data.data + component->data.size);
    return 1;
  } else if (component->type == httpRequestDtDataLast) {
    Context.Request.append(component->data.data, component->data.data + component->data.size);
    rapidjson::Document document;
    document.Parse(!Context.Request.empty() ? Context.Request.c_str() : "{}");
    if (document.HasParseError() || !document.IsObject()) {
      replyWithStatus("invalid_json");
      return 1;
    }

    switch (Context.function) {
      case fnUserAction: onUserAction(document); break;
      case fnUserCreate: onUserCreate(document); break;
      case fnUserResendEmail: onUserResendEmail(document); break;
      case fnUserLogin: onUserLogin(document); break;
      case fnUserLogout: onUserLogout(document); break;
      case fnUserQueryMonitoringSession: onUserQueryMonitoringSession(document); break;
      case fnUserChangeEmail: onUserChangeEmail(document); break;
      case fnUserChangePasswordInitiate: onUserChangePasswordInitiate(document); break;
      case fnUserChangePasswordForce: onUserChangePasswordForce(document); break;
      case fnUserGetCredentials: onUserGetCredentials(document); break;
      case fnUserGetSettings: onUserGetSettings(document); break;
      case fnUserUpdateCredentials: onUserUpdateCredentials(document); break;
      case fnUserUpdateSettings: onUserUpdateSettings(document); break;
      case fnUserEnumerateAll: onUserEnumerateAll(document); break;
      case fnUserEnumerateFeePlan: onUserEnumerateFeePlan(document); break;
      case fnUserGetFeePlan: onUserGetFeePlan(document); break;
      case fnUserUpdateFeePlan: onUserUpdateFeePlan(document); break;
      case fnUserChangeFeePlan: onUserChangeFeePlan(document); break;
      case fnUserActivate2faInitiate: onUserActivate2faInitiate(document); break;
      case fnUserDeactivate2faInitiate: onUserDeactivate2faInitiate(document); break;
      case fnBackendManualPayout: onBackendManualPayout(document); break;
      case fnBackendQueryUserBalance: onBackendQueryUserBalance(document); break;
      case fnBackendQueryUserStats: onBackendQueryUserStats(document); break;
      case fnBackendQueryUserStatsHistory: onBackendQueryUserStatsHistory(document); break;
      case fnBackendQueryWorkerStatsHistory: onBackendQueryWorkerStatsHistory(document); break;
      case fnBackendQueryCoins : onBackendQueryCoins(document); break;
      case fnBackendQueryFoundBlocks: onBackendQueryFoundBlocks(document); break;
      case fnBackendQueryPayouts: onBackendQueryPayouts(document); break;
      case fnBackendQueryPoolBalance: onBackendQueryPoolBalance(document); break;
      case fnBackendQueryPoolStats: onBackendQueryPoolStats(document); break;
      case fnBackendQueryPoolStatsHistory : onBackendQueryPoolStatsHistory(document); break;
      case fnBackendQueryProfitSwitchCoeff : onBackendQueryProfitSwitchCoeff(document); break;
      case fnBackendQueryPPLNSPayouts : onBackendQueryPPLNSPayouts(document); break;
      case fnBackendQueryPPLNSAcc : onBackendQueryPPLNSAcc(document); break;
      case fnBackendUpdateProfitSwitchCoeff : onBackendUpdateProfitSwitchCoeff(document); break;
      case fnBackendUpdatePPSConfig : onBackendUpdatePPSConfig(document); break;
      case fnBackendPoolLuck : onBackendPoolLuck(document); break;
      case fnInstanceEnumerateAll : onInstanceEnumerateAll(document); break;
      case fnComplexMiningStatsGetInfo : onComplexMiningStatsGetInfo(document); break;
      default:
        reply404();
        return 0;
    }
  }

  return 1;
}

void PoolHttpConnection::onWrite()
{
  // TODO: check keep alive
  socketShutdown(aioObjectSocket(Socket_), SOCKET_SHUTDOWN_READWRITE);
  aioRead(Socket_, buffer, sizeof(buffer), afNone, 0, readCb, this);
}

void PoolHttpConnection::onRead(AsyncOpStatus status, size_t bytesRead)
{
  if (status != aosSuccess) {
    close();
    return;
  }

  httpRequestSetBuffer(&ParserState, buffer, bytesRead + oldDataSize);

  switch (httpRequestParse(&ParserState, [](HttpRequestComponent *component, void *arg) -> int { return static_cast<PoolHttpConnection*>(arg)->onParse(component); }, this)) {
    case ParserResultOk : {
      // TODO: check keep-alive
      break;
    }

    case ParserResultNeedMoreData : {
      // copy 'tail' to begin of buffer
      oldDataSize = httpRequestDataRemaining(&ParserState);
      if (oldDataSize)
        memcpy(buffer, httpRequestDataPtr(&ParserState), oldDataSize);
      aioRead(Socket_, buffer+oldDataSize, sizeof(buffer)-oldDataSize, afNone, 0, readCb, this);
      break;
    }

    case ParserResultError : {
      close();
      break;
    }

    case ParserResultCancelled : {
      close();
      break;
    }
  }
}

void PoolHttpConnection::reply200(xmstream &stream)
{
  const char reply200[] = "HTTP/1.1 200 OK\r\nServer: bcnode\r\nTransfer-Encoding: chunked\r\n\r\n";
  stream.write(reply200, sizeof(reply200)-1);
}

void PoolHttpConnection::reply404()
{
  const char reply404[] = "HTTP/1.1 404 Not Found\r\nServer: bcnode\r\nTransfer-Encoding: chunked\r\n\r\n";
  const char html[] = "<html><head><title>Not Found</title></head><body><h1>404 Not Found</h1></body></html>";

  char buffer[4096];
  xmstream stream(buffer, sizeof(buffer));
  stream.write(reply404, sizeof(reply404)-1);

  size_t offset = startChunk(stream);
  stream.write(html);
  finishChunk(stream, offset);

  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

size_t PoolHttpConnection::startChunk(xmstream &stream)
{
  size_t offset = stream.offsetOf();
  stream.write("00000000\r\n", 10);
  return offset;
}

void PoolHttpConnection::finishChunk(xmstream &stream, size_t offset)
{
  char hex[16];
  char finishData[] = "\r\n0\r\n\r\n";
  snprintf(hex, sizeof(hex), "%08x", static_cast<unsigned>(stream.offsetOf() - offset - 10));
  memcpy(stream.data<uint8_t>() + offset, hex, 8);
  stream.write(finishData, sizeof(finishData));
}

void PoolHttpConnection::close()
{
  if (Deleted_++ == 0)
    deleteAioObject(Socket_);
}

void PoolHttpConnection::onUserAction(rapidjson::Document &document)
{
  std::string actionId;
  std::string newPassword;
  std::string totp;
  bool validAcc = true;
  jsonParseString(document, "actionId", actionId, &validAcc);
  jsonParseString(document, "newPassword", newPassword, "", &validAcc);
  jsonParseString(document, "totp", totp, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userAction(actionId, newPassword, totp, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserCreate(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  UserManager::Credentials credentials;

  jsonParseString(document, "id", sessionId, "", &validAcc);
  parseUserCredentials(document, credentials, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  UserManager::UserWithAccessRights tokenInfo;
  if (!sessionId.empty()) {
    if (!Server_.userManager().validateSession(sessionId, "", tokenInfo, false)) {
      replyWithStatus("unknown_id");
      return;
    }
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userCreate(tokenInfo.Login, std::move(credentials), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserResendEmail(rapidjson::Document &document)
{
  bool validAcc = true;
  UserManager::Credentials credentials;
  parseUserCredentials(document, credentials, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userResendEmail(std::move(credentials), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserLogin(rapidjson::Document &document)
{
  bool validAcc = true;
  UserManager::Credentials credentials;
  parseUserCredentials(document, credentials, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userLogin(std::move(credentials), [this](const std::string &sessionId, const char *status, bool isReadOnly) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    {
      JSON::Object result(stream);
      result.addString("status", status);
      result.addString("sessionid", sessionId);
      result.addBoolean("isReadOnly", isReadOnly);
    }

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserLogout(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  jsonParseString(document, "id", sessionId, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userLogout(sessionId, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserQueryMonitoringSession(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userQueryMonitoringSession(sessionId, targetLogin, [this](const std::string &sessionId, const char *status) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    {
      JSON::Object result(stream);
      result.addString("status", status);
      result.addString("sessionid", sessionId);
    }

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserChangeEmail(rapidjson::Document&)
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserChangePasswordInitiate(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string login;
  jsonParseString(document, "login", login, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userChangePasswordInitiate(login, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserChangePasswordForce(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string id;
  std::string login;
  std::string newPassword;
  jsonParseString(document, "id", id, &validAcc);
  jsonParseString(document, "login", login, &validAcc);
  jsonParseString(document, "newPassword", newPassword, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userChangePasswordForce(id, login, newPassword, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserGetCredentials(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  UserManager::UserWithAccessRights tokenInfo;
  UserManager::Credentials credentials;
  if (Server_.userManager().validateSession(sessionId, targetLogin, tokenInfo, false)) {
    JSON::Object result(stream);
    if (Server_.userManager().getUserCredentials(tokenInfo.Login, credentials)) {
      result.addString("status", "ok");
      result.addString("login", tokenInfo.Login);
      result.addString("name", credentials.Name);
      result.addString("email", credentials.EMail);
      result.addInt("registrationDate", credentials.RegistrationDate);
      result.addBoolean("isActive", credentials.IsActive);
      // We return readonly flag if user or session has it
      result.addBoolean("isReadOnly", tokenInfo.IsReadOnly | credentials.IsReadOnly);
      result.addBoolean("has2fa", credentials.HasTwoFactor);
    } else {
      result.addString("status", "unknown_id");
    }
  } else {
    JSON::Object result(stream);
    result.addString("status", "unknown_id");
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserGetSettings(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object object(stream);
    UserManager::UserWithAccessRights tokenInfo;
    if (Server_.userManager().validateSession(sessionId, targetLogin, tokenInfo, false)) {
      object.addString("status", "ok");
      object.addField("coins");
      JSON::Array coins(stream);
      for (const auto &coinInfo: Server_.userManager().coinInfo()) {
        coins.addField();
        JSON::Object coin(stream);
        UserSettingsRecord settings;
        coin.addString("name", coinInfo.Name.c_str());
        if (Server_.userManager().getUserCoinSettings(tokenInfo.Login, coinInfo.Name, settings)) {
          coin.addString("address", settings.Address);
          coin.addString("payoutThreshold", FormatMoney(settings.MinimalPayout, coinInfo.FractionalPartSize));
          coin.addBoolean("autoPayoutEnabled", settings.AutoPayout);
        } else {
          coin.addNull("address");
          coin.addNull("payoutThreshold");
          coin.addBoolean("autoPayoutEnabled", false);
        }
      }
    } else {
      object.addString("status", "unknown_id");
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserUpdateCredentials(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  UserManager::Credentials credentials;

  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  parseUserCredentials(document, credentials, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().updateCredentials(sessionId, targetLogin, std::move(credentials), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserUpdateSettings(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  UserManager::UserWithAccessRights tokenInfo;
  UserSettingsRecord settings;
  std::string payoutThreshold;
  std::string totp;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", settings.Coin, &validAcc);
  jsonParseString(document, "address", settings.Address, &validAcc);
  jsonParseString(document, "payoutThreshold", payoutThreshold, &validAcc);
  jsonParseBoolean(document, "autoPayoutEnabled", &settings.AutoPayout, &validAcc);
  jsonParseString(document, "totp", totp, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  auto It = Server_.userManager().coinIdxMap().find(settings.Coin);
  if (It == Server_.userManager().coinIdxMap().end()) {
    replyWithStatus("invalid_coin");
    return;
  }

  CCoinInfo &coinInfo = Server_.userManager().coinInfo()[It->second];
  if (!parseMoneyValue(payoutThreshold.c_str(), coinInfo.FractionalPartSize, &settings.MinimalPayout)) {
    replyWithStatus("request_format_error");
    return;
  }

  if (!coinInfo.checkAddress(settings.Address, coinInfo.PayoutAddressType)) {
    replyWithStatus("invalid_address");
    return;
  }

  if (!Server_.userManager().validateSession(sessionId, targetLogin, tokenInfo, true)) {
    replyWithStatus("unknown_id");
    return;
  }

  settings.Login = tokenInfo.Login;

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().updateSettings(std::move(settings), totp, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserEnumerateAll(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string coin;
  uint64_t offset = 0;
  uint64_t size = 0;
  std::string sortBy;
  bool sortDescending;

  jsonParseString(document, "id", sessionId, &validAcc);
  // TODO: remove sha256
  jsonParseString(document, "coin", coin, "sha256", &validAcc);
  jsonParseUInt64(document, "offset", &offset, 0, &validAcc);
  jsonParseUInt64(document, "size", &size, 100, &validAcc);
  jsonParseString(document, "sortBy", sortBy, "averagePower", &validAcc);
  jsonParseBoolean(document, "sortDescending", &sortDescending, true, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // sortBy convert
  StatisticDb::CredentialsWithStatistic::EColumns column;
  if (sortBy == "login") {
    column = StatisticDb::CredentialsWithStatistic::ELogin;
  } else if (sortBy == "workersNum") {
    column = StatisticDb::CredentialsWithStatistic::EWorkersNum;
  } else if (sortBy == "averagePower") {
    column = StatisticDb::CredentialsWithStatistic::EAveragePower;
  } else if (sortBy == "sharesPerSecond") {
    column = StatisticDb::CredentialsWithStatistic::ESharesPerSecond;
  } else if (sortBy == "lastShareTime") {
    column = StatisticDb::CredentialsWithStatistic::ELastShareTime;
  } else {
    replyWithStatus("unknown_column_name");
    return;
  }

  StatisticDb *statistic = Server_.statisticDb(coin);
  if (!statistic) {
    replyWithStatus("invalid_coin");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().enumerateUsers(sessionId, [this, statistic, offset, size, column, sortDescending](const char *status, std::vector<UserManager::Credentials> &allUsers) {
    statistic->queryAllusersStats(std::move(allUsers), [this, status](const std::vector<StatisticDb::CredentialsWithStatistic> &result) {
      xmstream stream;
      reply200(stream);
      size_t offset = startChunk(stream);

      {
        JSON::Object object(stream);
        object.addString("status", status);
        object.addField("users");
        {
          JSON::Array usersArray(stream);
          for (const auto &user: result) {
            usersArray.addField();
            {
              JSON::Object userObject(stream);
              userObject.addString("login", user.Credentials.Login);
              userObject.addString("name", user.Credentials.Name);
              userObject.addString("email", user.Credentials.EMail);
              userObject.addInt("registrationDate", user.Credentials.RegistrationDate);
              userObject.addBoolean("isActive", user.Credentials.IsActive);
              userObject.addBoolean("isReadOnly", user.Credentials.IsReadOnly);
              userObject.addString("feePlanId", user.Credentials.FeePlan);
              userObject.addInt("workers", user.WorkersNum);
              userObject.addDouble("shareRate", user.SharesPerSecond);
              userObject.addInt("power", user.AveragePower);
              userObject.addInt("lastShareTime", user.LastShareTime.toUnixTime());
            }
          }
        }
      }

      finishChunk(stream, offset);
      aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
      objectDecrementReference(aioObjectHandle(Socket_), 1);

    }, offset, size, column, sortDescending);
  });
}

void PoolHttpConnection::onUserUpdateFeePlan(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  UserFeePlanRecord record;

  auto jsonParseUserFeeConfig = [](rapidjson::Value &value, const char *fieldName, UserFeeConfig &config, bool *validAcc) {
    if (!value.HasMember(fieldName) || !value[fieldName].IsArray()) {
      *validAcc = false;
      return;
    }

    rapidjson::Value::Array pairs = value[fieldName].GetArray();
    for (rapidjson::SizeType i = 0, ie = pairs.Size(); i != ie; ++i) {
      if (!pairs[i].IsObject()) {
        *validAcc = false;
        return;
      }

      config.emplace_back();
      jsonParseString(pairs[i], "userId", config.back().UserId, validAcc);
      jsonParseNumber(pairs[i], "percentage", &config.back().Percentage, validAcc);
    }
  };

  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "feePlanId", record.FeePlanId, &validAcc);
  jsonParseUserFeeConfig(document, "default", record.Default, &validAcc);
  if (document.HasMember("coinSpecificFee") && document["coinSpecificFee"].IsArray()) {
    rapidjson::Value::Array coinSpecificFee = document["coinSpecificFee"].GetArray();
    for (rapidjson::SizeType i = 0, ie = coinSpecificFee.Size(); i != ie; ++i) {
      if (!coinSpecificFee[i].IsObject()) {
        validAcc = false;
        break;
      }

      record.CoinSpecificFee.emplace_back();
      jsonParseString(coinSpecificFee[i], "coin", record.CoinSpecificFee.back().CoinName, &validAcc);
      jsonParseUserFeeConfig(coinSpecificFee[i], "config", record.CoinSpecificFee.back().Config, &validAcc);
    }
  }

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // Check coin
  for (const auto &specificFee: record.CoinSpecificFee) {
    if (!Server_.backend(specificFee.CoinName)) {
      replyWithStatus("invalid_coin");
      return;
    }
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().updateFeePlan(sessionId, std::move(record), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserEnumerateFeePlan(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  jsonParseString(document, "id", sessionId, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  std::string status;
  std::vector<UserFeePlanRecord> result;
  if (Server_.userManager().enumerateFeePlan(sessionId, status, result)) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    {
      JSON::Object answer(stream);
      answer.addString("status", "ok");
      answer.addField("plans");
      {
        JSON::Array plans(stream);
        for (const auto &plan: result) {
          plans.addField();
          addUserFeePlan(stream, plan);
        }
      }
    }

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
  } else {
    replyWithStatus(status.c_str());
  }
}

void PoolHttpConnection::onUserGetFeePlan(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string feePlanId;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "feePlanId", feePlanId, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  std::string status;
  UserFeePlanRecord result;
  if (Server_.userManager().getFeePlan(sessionId, feePlanId, status, result)) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    {
      JSON::Object answer(stream);
      answer.addString("status", "ok");
      answer.addField("plan");
      addUserFeePlan(stream, result);
    }

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
  } else {
    replyWithStatus(status.c_str());
  }
}

void PoolHttpConnection::onUserChangeFeePlan(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  std::string feePlanId;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, &validAcc);
  jsonParseString(document, "feePlanId", feePlanId, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().changeFeePlan(sessionId, targetLogin, feePlanId, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserActivate2faInitiate(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  jsonParseString(document, "sessionId", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().activate2faInitiate(sessionId, targetLogin, [this](const char *status, const char *key) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    {
      JSON::Object result(stream);
      result.addString("status", status);
      result.addString("key", key);
    }

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserDeactivate2faInitiate(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  jsonParseString(document, "sessionId", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().deactivate2faInitiate(sessionId, targetLogin, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendManualPayout(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // id -> login
  UserManager::UserWithAccessRights login;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, login, true)) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend->accountingDb()->manualPayout(login.Login, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryUserBalance(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // id -> login
  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, tokenInfo, false)) {
    replyWithStatus("unknown_id");
    return;
  }

  if (!coin.empty()) {
    PoolBackend *backend = Server_.backend(coin);
    if (!backend) {
      replyWithStatus("invalid_coin");
      return;
    }

    objectIncrementReference(aioObjectHandle(Socket_), 1);
    backend->accountingDb()->queryUserBalance(tokenInfo.Login, [this, backend](const AccountingDb::UserBalanceInfo &record) {
      xmstream stream;
      reply200(stream);
      size_t offset = startChunk(stream);
      const CCoinInfo &coinInfo = backend->getCoinInfo();
      {
        JSON::Object object(stream);
        object.addString("status", "ok");
        object.addField("balances");
        {
          JSON::Array allBalances(stream);
          allBalances.addField();
          {
            JSON::Object balance(stream);
            balance.addString("coin", coinInfo.Name);
            balance.addString("balance", FormatMoney(record.Data.Balance, coinInfo.FractionalPartSize));
            balance.addString("requested", FormatMoney(record.Data.Requested, coinInfo.FractionalPartSize));
            balance.addString("paid", FormatMoney(record.Data.Paid, coinInfo.FractionalPartSize));
            balance.addString("queued", FormatMoney(record.Queued, coinInfo.FractionalPartSize));
          }
        }
      }

      finishChunk(stream, offset);
      aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
  } else {
    // Ask all backends about balances
    objectIncrementReference(aioObjectHandle(Socket_), 1);

    std::vector<AccountingDb*> accountingDbs(Server_.backends().size());
    for (size_t i = 0, ie = Server_.backends().size(); i != ie; ++i)
      accountingDbs[i] = Server_.backend(i)->accountingDb();

    AccountingDb::queryUserBalanceMulti(&accountingDbs[0], accountingDbs.size(), tokenInfo.Login, [this](const AccountingDb::UserBalanceInfo *balanceData, size_t backendsNum) {
      xmstream stream;
      reply200(stream);
      size_t offset = startChunk(stream);
      {
        JSON::Object object(stream);
        object.addString("status", "ok");
        object.addField("balances");
        {
          JSON::Array allBalances(stream);
          for (size_t i = 0; i < backendsNum; i++) {
            const CCoinInfo &coinInfo = Server_.backend(i)->getCoinInfo();
            allBalances.addField();
            {
              JSON::Object balance(stream);
              balance.addString("coin", coinInfo.Name);
              balance.addString("balance", FormatMoney(balanceData[i].Data.Balance, coinInfo.FractionalPartSize));
              balance.addString("requested", FormatMoney(balanceData[i].Data.Requested, coinInfo.FractionalPartSize));
              balance.addString("paid", FormatMoney(balanceData[i].Data.Paid, coinInfo.FractionalPartSize));
              balance.addString("queued", FormatMoney(balanceData[i].Queued, coinInfo.FractionalPartSize));
            }
          }
        }
      }

      finishChunk(stream, offset);
      aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
  }
}

void PoolHttpConnection::onBackendQueryUserStats(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  uint64_t offset = 0;
  uint64_t size = 0;
  std::string sortBy;
  bool sortDescending;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, "", &validAcc);
  jsonParseUInt64(document, "offset", &offset, 0, &validAcc);
  jsonParseUInt64(document, "size", &size, 4096, &validAcc);
  jsonParseString(document, "sortBy", sortBy, "name", &validAcc);
  jsonParseBoolean(document, "sortDescending", &sortDescending, false, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // sortBy convert
  StatisticDb::EStatsColumn column;
  if (sortBy == "name") {
    column = StatisticDb::EStatsColumnName;
  } else if (sortBy == "averagePower") {
    column = StatisticDb::EStatsColumnAveragePower;
  } else if (sortBy == "sharesPerSecond") {
    column = StatisticDb::EStatsColumnSharesPerSecond;
  } else if (sortBy == "lastShareTime") {
    column = StatisticDb::EStatsColumnLastShareTime;
  } else {
    replyWithStatus("unknown_column_name");
    return;
  }

  // id -> login
  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, tokenInfo, false)) {
    replyWithStatus("unknown_id");
    return;
  }

  StatisticDb *statistic = Server_.statisticDb(coin);
  if (!statistic) {
    replyWithStatus("invalid_coin");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  statistic->queryUserStats(tokenInfo.Login, [this, statistic](const CStats &aggregate, const std::vector<CStats> &workers) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    {
      JSON::Object object(stream);
      object.addString("status", "ok");
      object.addString("powerUnit", statistic->getCoinInfo().getPowerUnitName());
      object.addInt("powerMultLog10", statistic->getCoinInfo().PowerMultLog10);
      object.addInt("currentTime", time(nullptr));
      object.addField("total");
      {
        JSON::Object total(stream);
        total.addInt("clients", aggregate.ClientsNum);
        total.addInt("workers", aggregate.WorkersNum);
        total.addDouble("shareRate", aggregate.SharesPerSecond);
        total.addString("shareWork", aggregate.SharesWork.getDecimal());
        total.addInt("power", aggregate.AveragePower);
        total.addInt("lastShareTime", aggregate.LastShareTime.toUnixTime());
      }

      object.addField("workers");
      {
        JSON::Array workersOutput(stream);
        for (size_t i = 0, ie = workers.size(); i != ie; ++i) {
          workersOutput.addField();
          {
            JSON::Object workerOutput(stream);
            workerOutput.addString("name", workers[i].WorkerId);
            workerOutput.addDouble("shareRate", workers[i].SharesPerSecond);
            workerOutput.addString("shareWork", workers[i].SharesWork.getDecimal());
            workerOutput.addInt("power", workers[i].AveragePower);
            workerOutput.addInt("lastShareTime", workers[i].LastShareTime.toUnixTime());
          }
        }
      }
    }

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  }, offset, size, column, sortDescending);
}

void PoolHttpConnection::queryStatsHistory(StatisticDb *statistic, const std::string &login, const std::string &worker, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, int64_t currentTime)
{
  std::vector<CStats> stats;
  statistic->getHistory(login, worker, timeFrom, timeTo, groupByInterval, stats);

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object object(stream);
    object.addString("status", "ok");
    object.addString("powerUnit", statistic->getCoinInfo().getPowerUnitName());
    object.addInt("powerMultLog10", statistic->getCoinInfo().PowerMultLog10);
    object.addInt("currentTime", currentTime);
    object.addField("stats");
    {
      JSON::Array workersOutput(stream);
      for (size_t i = 0, ie = stats.size(); i != ie; ++i) {
        workersOutput.addField();
        {
          JSON::Object workerOutput(stream);
          workerOutput.addString("name", stats[i].WorkerId);
          workerOutput.addInt("time", stats[i].Time.toUnixTime());
          workerOutput.addDouble("shareRate", stats[i].SharesPerSecond);
          workerOutput.addString("shareWork", stats[i].SharesWork.getDecimal());
          workerOutput.addInt("power", stats[i].AveragePower);
        }
      }
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onBackendQueryUserStatsHistory(rapidjson::Document &document)
{
  bool validAcc = true;
  int64_t currentTime = time(nullptr);
  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  int64_t timeFrom;
  int64_t timeTo;
  int64_t groupByInterval;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, "", &validAcc);
  jsonParseInt64(document, "timeFrom", &timeFrom, currentTime - 24*3600, &validAcc);
  jsonParseInt64(document, "timeTo", &timeTo, currentTime, &validAcc);
  jsonParseInt64(document, "groupByInterval", &groupByInterval, 3600, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // id -> login
  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, tokenInfo, false)) {
    replyWithStatus("unknown_id");
    return;
  }

  StatisticDb *statistic = Server_.statisticDb(coin);
  if (!statistic) {
    replyWithStatus("invalid_coin");
    return;
  }

  queryStatsHistory(statistic, tokenInfo.Login, "", timeFrom, timeTo, groupByInterval, currentTime);
}

void PoolHttpConnection::onBackendQueryWorkerStatsHistory(rapidjson::Document &document)
{
  bool validAcc = true;
  int64_t currentTime = time(nullptr);
  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  std::string workerId;
  int64_t timeFrom;
  int64_t timeTo;
  int64_t groupByInterval;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, &validAcc);
  jsonParseString(document, "workerId", workerId, &validAcc);
  jsonParseInt64(document, "timeFrom", &timeFrom, currentTime - 24*3600, &validAcc);
  jsonParseInt64(document, "timeTo", &timeTo, currentTime, &validAcc);
  jsonParseInt64(document, "groupByInterval", &groupByInterval, 3600, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // id -> login
  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, tokenInfo, false)) {
    replyWithStatus("unknown_id");
    return;
  }

  StatisticDb *statistic = Server_.statisticDb(coin);
  if (!statistic) {
    replyWithStatus("invalid_coin");
    return;
  }

  queryStatsHistory(statistic, tokenInfo.Login, workerId, timeFrom, timeTo, groupByInterval, currentTime);
}

void PoolHttpConnection::onBackendQueryCoins(rapidjson::Document&)
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  {
    JSON::Object result(stream);
    result.addString("status", "ok");
    result.addField("coins");
    JSON::Array coins(stream);
    for (const auto &backend: Server_.backends()) {
      coins.addField();
      {
        JSON::Object object(stream);
        const CCoinInfo &info = backend->getCoinInfo();
        object.addString("name", info.Name);
        object.addString("fullName", info.FullName);
        object.addString("algorithm", info.Algorithm);
        object.addString("minimalPayout", FormatMoney(backend->getConfig().MinimalAllowedPayout, info.FractionalPartSize));
        // TODO: calculate fee for current user
        object.addDouble("totalFee", 0.0);
      }
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onBackendQueryFoundBlocks(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string coin;
  int64_t heightFrom;
  std::string hashFrom;
  uint32_t count;
  jsonParseString(document, "coin", coin, &validAcc);
  jsonParseInt64(document, "heightFrom", &heightFrom, -1, &validAcc);
  jsonParseString(document, "hashFrom", hashFrom, "", &validAcc);
  jsonParseUInt(document, "count", &count, 20, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  const CCoinInfo &coinInfo = backend->getCoinInfo();

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend->accountingDb()->queryFoundBlocks(heightFrom, hashFrom, count, [this, &coinInfo](const std::vector<FoundBlockRecord> &blocks, const std::vector<CNetworkClient::GetBlockConfirmationsQuery> &confirmations) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);
    {
      JSON::Object response(stream);
      response.addString("status", "ok");
      response.addField("blocks");
      {
        JSON::Array blocksArray(stream);
        for (size_t i = 0, ie = blocks.size(); i != ie; ++i) {
          blocksArray.addField();
          {
            JSON::Object block(stream);
            block.addInt("height", blocks[i].Height);
            block.addString("hash", !blocks[i].PublicHash.empty() ? blocks[i].PublicHash : blocks[i].Hash);
            block.addInt("time", blocks[i].Time.toUnixTime());
            block.addInt("confirmations", confirmations[i].Confirmations);
            block.addString("generatedCoins", FormatMoney(blocks[i].AvailableCoins, coinInfo.FractionalPartSize));
            block.addString("foundBy", blocks[i].FoundBy);
          }
        }
      }
    }

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryPayouts(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  uint64_t timeFrom = 0;
  unsigned count;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, "", &validAcc);
  jsonParseUInt64(document, "timeFrom", &timeFrom, 0, &validAcc);
  jsonParseUInt(document, "count", &count, 20, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // id -> login
  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, tokenInfo, false)) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  std::vector<PayoutDbRecord> records;
  backend->queryPayouts(tokenInfo.Login, timeFrom, count, records);
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object response(stream);
    response.addString("status", "ok");
    response.addField("payouts");
    {
      JSON::Array payoutsArray(stream);
      for (size_t i = 0, ie = records.size(); i != ie; ++i) {
        payoutsArray.addField();
        {
          JSON::Object payout(stream);
          payout.addInt("time", records[i].Time);
          payout.addString("txid", records[i].TransactionId);
          payout.addString("value", FormatMoney(records[i].Value, backend->getCoinInfo().FractionalPartSize));
          payout.addInt("status", records[i].Status);
        }
      }
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onBackendQueryPoolBalance(rapidjson::Document&)
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onBackendQueryPoolStats(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string coin;
  jsonParseString(document, "coin", coin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  if (!coin.empty()) {
    StatisticDb *statistic = Server_.statisticDb(coin);
    if (!statistic) {
      replyWithStatus("invalid_coin");
      return;
    }

    objectIncrementReference(aioObjectHandle(Socket_), 1);
    statistic->queryPoolStats([this, statistic](const CStats &record) {
      xmstream stream;
      reply200(stream);
      size_t offset = startChunk(stream);
      const CCoinInfo &coinInfo = statistic->getCoinInfo();

      {
        JSON::Object object(stream);
        object.addString("status", "ok");
        object.addInt("currentTime", time(nullptr));
        object.addField("stats");
        {
          JSON::Array statsArray(stream);
          statsArray.addField();
          {
            JSON::Object statsObject(stream);
            statsObject.addString("coin", coinInfo.Name);
            statsObject.addString("powerUnit", statistic->getCoinInfo().getPowerUnitName());
            statsObject.addInt("powerMultLog10", statistic->getCoinInfo().PowerMultLog10);
            statsObject.addInt("clients", record.ClientsNum);
            statsObject.addInt("workers", record.WorkersNum);
            statsObject.addDouble("shareRate", record.SharesPerSecond);
            statsObject.addString("shareWork", record.SharesWork.getDecimal());
            statsObject.addInt("power", record.AveragePower);
            statsObject.addInt("lastShareTime", record.LastShareTime.toUnixTime());
          }
        }
      }

      finishChunk(stream, offset);
      aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
  } else {
    // Ask all backends about stats
    objectIncrementReference(aioObjectHandle(Socket_), 1);

    std::vector<StatisticDb*> statisticDbs(Server_.backends().size());
    for (size_t i = 0, ie = Server_.backends().size(); i != ie; ++i)
      statisticDbs[i] = Server_.backend(i)->statisticDb();

    StatisticDb::queryPoolStatsMulti(&statisticDbs[0], statisticDbs.size(), [this](const CStats *stats, size_t backendsNum) {
      xmstream stream;
      reply200(stream);
      size_t offset = startChunk(stream);

      {
        JSON::Object object(stream);
        object.addString("status", "ok");
        object.addField("stats");
        {
          JSON::Array statsArray(stream);
          for (size_t i = 0; i < backendsNum; i++) {
            const CCoinInfo &coinInfo = Server_.backend(i)->getCoinInfo();
            statsArray.addField();
            {
              JSON::Object statsObject(stream);
              statsObject.addString("coin", coinInfo.Name);
              statsObject.addString("powerUnit", coinInfo.getPowerUnitName());
              statsObject.addInt("powerMultLog10", coinInfo.PowerMultLog10);
              statsObject.addInt("clients", stats[i].ClientsNum);
              statsObject.addInt("workers", stats[i].WorkersNum);
              statsObject.addDouble("shareRate", stats[i].SharesPerSecond);
              statsObject.addString("shareWork", stats[i].SharesWork.getDecimal());
              statsObject.addInt("power", stats[i].AveragePower);
            }
          }
        }
      }

      finishChunk(stream, offset);
      aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
  }
}

void PoolHttpConnection::onBackendQueryPoolStatsHistory(rapidjson::Document &document)
{
  bool validAcc = true;
  int64_t currentTime = time(nullptr);
  std::string coin;
  int64_t timeFrom;
  int64_t timeTo;
  int64_t groupByInterval;
  jsonParseString(document, "coin", coin, "", &validAcc);
  jsonParseInt64(document, "timeFrom", &timeFrom, currentTime - 24*3600, &validAcc);
  jsonParseInt64(document, "timeTo", &timeTo, currentTime, &validAcc);
  jsonParseInt64(document, "groupByInterval", &groupByInterval, 3600, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  StatisticDb *statistic = Server_.statisticDb(coin);
  if (!statistic) {
    replyWithStatus("invalid_coin");
    return;
  }

  queryStatsHistory(statistic, "", "", timeFrom, timeTo, groupByInterval, currentTime);
}

void PoolHttpConnection::onBackendQueryProfitSwitchCoeff(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  jsonParseString(document, "id", sessionId, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, "", tokenInfo, false) || (tokenInfo.Login != "admin" && tokenInfo.Login != "observer")) {
    replyWithStatus("unknown_id");
    return;
  }

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  {
    JSON::Object result(stream);
    result.addString("status", "ok");
    result.addField("coins");
    {
      JSON::Array coins(stream);
      for (const auto &backend: Server_.backends()) {
        coins.addField();
        {
          JSON::Object object(stream);
          const CCoinInfo &info = backend->getCoinInfo();
          object.addString("name", info.Name);
          object.addDouble("profitSwitchCoeff", backend->getProfitSwitchCoeff());
        }
      }
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onBackendQueryPPLNSPayouts(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  int64_t timeFrom;
  std::string hashFrom;
  uint32_t count;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, &validAcc);
  jsonParseInt64(document, "timeFrom", &timeFrom, 0, &validAcc);
  jsonParseString(document, "hashFrom", hashFrom, "", &validAcc);
  jsonParseUInt(document, "count", &count, 20, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, tokenInfo, false)) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend->accountingDb()->queryPPLNSPayouts(tokenInfo.Login, timeFrom, hashFrom, count, [this, backend](const std::vector<CPPLNSPayout>& result) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);
    {
      JSON::Object response(stream);
      response.addString("status", "ok");
      response.addField("payouts");
      {
        JSON::Array payoutArray(stream);
        for (const auto &payout: result) {
          payoutArray.addField();
          {
            JSON::Object payoutObject(stream);
            payoutObject.addInt("startTime", payout.RoundStartTime.toUnixTime());
            payoutObject.addInt("endTime", payout.RoundEndTime.toUnixTime());
            payoutObject.addString("hash", payout.BlockHash);
            payoutObject.addInt("height", payout.BlockHeight);
            payoutObject.addString("value", FormatMoney(payout.PayoutValue, backend->getCoinInfo().FractionalPartSize));
            payoutObject.addDouble("coinBtcRate", fnormalize(payout.RateToBTC));
            payoutObject.addDouble("btcUsdRate", fnormalize(payout.RateBTCToUSD));
          }
        }
      }
    }
    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryPPLNSAcc(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  int64_t timeFrom;
  int64_t timeTo;
  int64_t groupByInterval;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, &validAcc);
  jsonParseInt64(document, "timeFrom", &timeFrom, &validAcc);
  jsonParseInt64(document, "timeTo", &timeTo, &validAcc);
  jsonParseInt64(document, "groupByInterval", &groupByInterval, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, tokenInfo, false)) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  // Check intervals
  if (timeTo <= timeFrom ||
      groupByInterval == 0 ||
      (timeTo - timeFrom) % groupByInterval != 0 ||
      (timeTo - timeFrom) / groupByInterval > 3200) {
    replyWithStatus("invalid_interval");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend->accountingDb()->queryPPLNSAcc(tokenInfo.Login, timeFrom, timeTo, groupByInterval, [this, backend](const std::vector<AccountingDb::CPPLNSPayoutAcc>& result) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);
    {
      JSON::Object response(stream);
      response.addString("status", "ok");
      response.addField("payouts");
      {
        JSON::Array payoutArray(stream);
        for (const auto &payout: result) {
          payoutArray.addField();
          {
            JSON::Object payoutObject(stream);
            payoutObject.addInt("timeLabel", payout.IntervalEnd);
            payoutObject.addString("value", FormatMoney(payout.TotalCoin, backend->getCoinInfo().FractionalPartSize));
            payoutObject.addString("valueBTC", FormatMoney(payout.TotalBTC, 8));
            payoutObject.addString("valueUSD", FormatMoney(payout.TotalUSD, 8));
            payoutObject.addInt("avghashrate", payout.AvgHashRate);
          }
        }
      }
    }
    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendUpdateProfitSwitchCoeff(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string coin;
  double profitSwitchCoeff = 0.0;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "coin", coin, "", &validAcc);
  jsonParseNumber(document, "profitSwitchCoeff", &profitSwitchCoeff, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, "", tokenInfo, false) || (tokenInfo.Login != "admin")) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  backend->setProfitSwitchCoeff(profitSwitchCoeff);
  replyWithStatus("ok");
}

void PoolHttpConnection::onBackendUpdatePPSConfig(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string coin;
  bool ppsModeEnabled = false;
  double ppsPoolFee = 4.0;
  std::string ppsSaturationFunction;
  double ppsSaturationB0 = 0.0;
  double ppsSaturationANegative = 0.0;
  double ppsSaturationAPositive = 0.0;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "coin", coin, &validAcc);
  jsonParseBoolean(document, "ppsModeEnabled", &ppsModeEnabled, &validAcc);
  jsonParseNumber(document, "ppsPoolFee", &ppsPoolFee, &validAcc);
  jsonParseString(document, "ppsSaturationFunction", ppsSaturationFunction, "", &validAcc);
  jsonParseNumber(document, "ppsSaturationB0", &ppsSaturationB0, &validAcc);
  jsonParseNumber(document, "ppsSaturationANegative", &ppsSaturationANegative, &validAcc);
  jsonParseNumber(document, "ppsSaturationAPositive", &ppsSaturationAPositive, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, "", tokenInfo, false) || (tokenInfo.Login != "admin")) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  ESaturationFunction saturationFunction;
  if (!CPPSConfig::parseSaturationFunction(ppsSaturationFunction, &saturationFunction)) {
    replyWithStatus("invalid_saturation_function");
    return;
  }

  CPPSConfig cfg;
  cfg.PoolFee = ppsPoolFee;
  cfg.SaturationFunction = saturationFunction;
  cfg.SaturationB0 = ppsSaturationB0;
  cfg.SaturationANegative = ppsSaturationANegative;
  cfg.SaturationAPositive = ppsSaturationAPositive;

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend->accountingDb()->updatePPSConfig(ppsModeEnabled, cfg, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendPoolLuck(rapidjson::Document &document)
{
  if (!document.HasMember("coin") || !document["coin"].IsString() ||
      !document.HasMember("intervals") || !document["intervals"].IsArray()) {
    replyWithStatus("json_format_error");
    return;
  }

  std::string coin = document["coin"].GetString();
  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  int64_t prevInterval = 0;
  std::vector<int64_t> intervals;
  rapidjson::Value::Array intervalsValue = document["intervals"].GetArray();
  for (rapidjson::SizeType i = 0, ie = intervalsValue.Size(); i != ie; ++i) {
    if (!intervalsValue[i].IsInt64()) {
      replyWithStatus("json_format_error");
      return;
    }

    int64_t interval = intervalsValue[i].GetInt64();
    if (interval <= prevInterval) {
      replyWithStatus("json_format_error");
      return;
    }

    prevInterval = interval;
    intervals.push_back(interval);
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend->accountingDb()->poolLuck(std::move(intervals), [this](const std::vector<double> &result) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);
    {
      JSON::Object response(stream);
      response.addString("status", "ok");
      response.addField("luck");
      {
        JSON::Array luckArray(stream);
        for (const auto &luck: result)
          luckArray.addDouble(luck);
      }
    }

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onInstanceEnumerateAll(rapidjson::Document&)
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  {
    JSON::Object result(stream);
    result.addString("status", "ok");
    result.addField("instances");
    {
      JSON::Array instances(stream);
      for (const auto &instance: Server_.config().Instances) {
        instances.addField();
        JSON::Object instanceObject(stream);
        instanceObject.addString("protocol", instance.Protocol);
        instanceObject.addString("type", instance.Type);
        instanceObject.addInt("port", instance.Port);
        instanceObject.addField("backends");
        {
          JSON::Array backends(stream);
          for (const auto &backend: instance.Backends)
            backends.addString(backend);
        }
        if (instance.Protocol == "stratum")
          instanceObject.addDouble("shareDiff", instance.StratumShareDiff);
      }
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onComplexMiningStatsGetInfo(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  jsonParseString(document, "id", sessionId, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, "", tokenInfo, false) || (tokenInfo.Login != "admin")) {
    replyWithStatus("unknown_id");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.miningStats().query(document, [this](const char *data, size_t size) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);
    stream.write(data, size);
    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

PoolHttpServer::PoolHttpServer(uint16_t port,
                               UserManager &userMgr,
                               std::vector<std::unique_ptr<PoolBackend>> &backends,
                               std::vector<std::unique_ptr<StatisticServer>> &algoMetaStatistic,
                               ComplexMiningStats &complexMiningStats,
                               const CPoolFrontendConfig &config,
                               size_t threadsNum) :
  Port_(port),
  UserMgr_(userMgr),
  MiningStats_(complexMiningStats),
  Config_(config),
  ThreadsNum_(threadsNum)
{
  Base_ = createAsyncBase(amOSDefault);
  for (size_t i = 0, ie = backends.size(); i != ie; ++i) {
    Backends_.push_back(backends[i].get());
    Statistic_.push_back(backends[i]->statisticDb());
  }

  for (size_t  i = 0, ie = algoMetaStatistic.size(); i != ie; ++i)
    Statistic_.push_back(algoMetaStatistic[i]->statisticDb());

  std::sort(Backends_.begin(), Backends_.end(), [](const auto &l, const auto &r) { return l->getCoinInfo().Name < r->getCoinInfo().Name; });
  std::sort(Statistic_.begin(), Statistic_.end(), [](const auto &l, const auto &r) { return l->getCoinInfo().Name < r->getCoinInfo().Name; });
}

bool PoolHttpServer::start()
{
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(Port_);
  socketTy hSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  socketReuseAddr(hSocket);

  if (socketBind(hSocket, &address) != 0) {
    LOG_F(ERROR, "PoolHttpServer: can't bind port %u\n", static_cast<unsigned>(Port_));
    return false;
  }

  if (socketListen(hSocket) != 0) {
    LOG_F(ERROR, "PoolHttpServer: can't listen port %u\n", static_cast<unsigned>(Port_));
    return false;
  }

  ListenerSocket_ = newSocketIo(Base_, hSocket);
  aioAccept(ListenerSocket_, 0, acceptCb, this);

  Threads_.reset(new std::thread[ThreadsNum_]);
  for (size_t i = 0; i < ThreadsNum_; i++) {
    Threads_[i] = std::thread([i](PoolHttpServer *server) {
      char threadName[16];
      snprintf(threadName, sizeof(threadName), "http%zu", i);
      loguru::set_thread_name(threadName);
      InitializeWorkerThread();
      LOG_F(INFO, "http server started tid=%u", GetGlobalThreadId());
      asyncLoop(server->Base_);
    }, this);
  }

  return true;
}

void PoolHttpServer::stop()
{
  postQuitOperation(Base_);
  for (size_t i = 0; i < ThreadsNum_; i++) {
    LOG_F(INFO, "http worker %zu finishing", i);
    Threads_[i].join();
  }
}


void PoolHttpServer::acceptCb(AsyncOpStatus status, aioObject *object, HostAddress, socketTy socketFd, void *arg)
{
  if (status == aosSuccess) {
    aioObject *connectionSocket = newSocketIo(aioGetBase(object), socketFd);
    PoolHttpConnection *connection = new PoolHttpConnection(*static_cast<PoolHttpServer*>(arg), connectionSocket);
    connection->run();
  } else {
    LOG_F(ERROR, "HTTP api accept connection failed");
  }

  aioAccept(object, 0, acceptCb, arg);
}

void PoolHttpConnection::replyWithStatus(const char *status)
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object object(stream);
    object.addString("status", status);
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}
