#include "http.h"
#include "poolcommon/utils.h"
#include "poolcore/priceFetcher.h"
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
  {"userCreateFeePlan", {hmPost, fnUserCreateFeePlan}},
  {"userQueryFeePlan", {hmPost, fnUserQueryFeePlan}},
  {"userUpdateFeePlan", {hmPost, fnUserUpdateFeePlan}},
  {"userDeleteFeePlan", {hmPost, fnUserDeleteFeePlan}},
  {"userChangeFeePlan", {hmPost, fnUserChangeFeePlan}},
  {"userRenewFeePlanReferralId", {hmPost, fnUserRenewFeePlanReferralId}},
  {"userActivate2faInitiate", {hmPost, fnUserActivate2faInitiate}},
  {"userDeactivate2faInitiate", {hmPost, fnUserDeactivate2faInitiate}},
  {"userAdjustInstantPayoutThreshold", {hmPost, fnUserAdjustInstantPayoutThreshold}},
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
  {"backendQueryPPSPayouts", {hmPost, fnBackendQueryPPSPayouts}},
  {"backendQueryPPSPayoutsAcc", {hmPost, fnBackendQueryPPSPayoutsAcc}},
  {"backendUpdateProfitSwitchCoeff", {hmPost, fnBackendUpdateProfitSwitchCoeff}},
  {"backendGetConfig", {hmPost, fnBackendGetConfig}},
  {"backendGetPPSState", {hmPost, fnBackendGetPPSState}},
  {"backendQueryPPSHistory", {hmPost, fnBackendQueryPPSHistory}},
  {"backendUpdateConfig", {hmPost, fnBackendUpdateConfig}},
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

static inline void jsonParseNumber(rapidjson::Value &document, const char *name, double *out, double defaultValue, bool *validAcc) {
  if (document.HasMember(name)) {
    if (document[name].IsNumber())
      *out = document[name].GetDouble();
    else
      *validAcc = false;
  } else {
    *out = defaultValue;
  }
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
  jsonParseString(document, "referralId", credentials.ReferralId, "", validAcc);
}

static void addUserFeeConfig(xmstream &stream, const std::vector<UserFeePair> &config)
{
  JSON::Array cfg(stream);
  for (const auto &pair: config) {
    cfg.addField();
    JSON::Object pairObject(stream);
    pairObject.addString("userId", pair.UserId);
    pairObject.addDouble("percentage", pair.Percentage);
  }
}

static void addModeFeeConfigFields(xmstream &stream, JSON::Object &parent, const CModeFeeConfig &modeCfg)
{
  parent.addField("default");
  addUserFeeConfig(stream, modeCfg.Default);
  parent.addField("coinSpecific");
  {
    JSON::Array coinArray(stream);
    for (const auto &cfg: modeCfg.CoinSpecific) {
      coinArray.addField();
      JSON::Object coin(stream);
      coin.addString("coinName", cfg.CoinName);
      coin.addField("config");
      addUserFeeConfig(stream, cfg.Config);
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
      case fnUserCreateFeePlan: onUserCreateFeePlan(document); break;
      case fnUserQueryFeePlan: onUserQueryFeePlan(document); break;
      case fnUserUpdateFeePlan: onUserUpdateFeePlan(document); break;
      case fnUserDeleteFeePlan: onUserDeleteFeePlan(document); break;
      case fnUserChangeFeePlan: onUserChangeFeePlan(document); break;
      case fnUserRenewFeePlanReferralId: onUserRenewFeePlanReferralId(document); break;
      case fnUserActivate2faInitiate: onUserActivate2faInitiate(document); break;
      case fnUserDeactivate2faInitiate: onUserDeactivate2faInitiate(document); break;
      case fnUserAdjustInstantPayoutThreshold: onUserAdjustInstantPayoutThreshold(document); break;
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
      case fnBackendQueryPPSPayouts : onBackendQueryPPSPayouts(document); break;
      case fnBackendQueryPPSPayoutsAcc : onBackendQueryPPSPayoutsAcc(document); break;
      case fnBackendUpdateProfitSwitchCoeff : onBackendUpdateProfitSwitchCoeff(document); break;
      case fnBackendGetConfig : onBackendGetConfig(document); break;
      case fnBackendGetPPSState : onBackendGetPPSState(document); break;
      case fnBackendQueryPPSHistory : onBackendQueryPPSHistory(document); break;
      case fnBackendUpdateConfig : onBackendUpdateConfig(document); break;
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

  if (!credentials.FeePlan.empty() && !credentials.ReferralId.empty()) {
    replyWithStatus("request_format_error");
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
        Server_.userManager().getUserCoinSettings(tokenInfo.Login, coinInfo.Name, settings);
        coin.addField("payout");
        {
          JSON::Object payout(stream);
          payout.addString("mode", payoutModeName(settings.Payout.Mode));
          payout.addString("address", settings.Payout.Address);
          payout.addString("instantPayoutThreshold", FormatMoney(settings.Payout.InstantPayoutThreshold, coinInfo.FractionalPartSize));
        }
        coin.addField("mining");
        {
          JSON::Object mining(stream);
          mining.addString("mode", miningModeName(settings.Mining.MiningMode));
        }
        coin.addField("autoExchange");
        {
          JSON::Object autoExchange(stream);
          autoExchange.addString("payoutCoinName", settings.AutoExchange.PayoutCoinName);
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
  std::string coin;
  std::string totp;

  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, &validAcc);
  jsonParseString(document, "totp", totp, "", &validAcc);

  bool hasPayout = document.HasMember("payout");
  CSettingsPayout settingsPayout;
  std::string payoutMode;
  std::string instantPayoutThreshold;
  if (hasPayout) {
    if (!document["payout"].IsObject()) {
      validAcc = false;
    } else {
      rapidjson::Value &payoutValue = document["payout"];
      jsonParseString(payoutValue, "mode", payoutMode, &validAcc);
      jsonParseString(payoutValue, "address", settingsPayout.Address, &validAcc);
      jsonParseString(payoutValue, "instantPayoutThreshold", instantPayoutThreshold, &validAcc);
    }
  }

  bool hasMining = document.HasMember("mining");
  std::string miningMode;
  if (hasMining) {
    if (!document["mining"].IsObject()) {
      validAcc = false;
    } else {
      rapidjson::Value &miningValue = document["mining"];
      jsonParseString(miningValue, "mode", miningMode, &validAcc);
    }
  }

  bool hasAutoExchange = document.HasMember("autoExchange");
  CSettingsAutoExchange settingsAutoExchange;
  if (hasAutoExchange) {
    if (!document["autoExchange"].IsObject()) {
      validAcc = false;
    } else {
      rapidjson::Value &autoExchangeValue = document["autoExchange"];
      jsonParseString(autoExchangeValue, "payoutCoinName", settingsAutoExchange.PayoutCoinName, &validAcc);
    }
  }

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  if (!hasPayout && !hasMining && !hasAutoExchange) {
    replyWithStatus("json_format_error");
    return;
  }

  std::optional<CSettingsPayout> payout;
  std::optional<CSettingsMining> mining;
  std::optional<CSettingsAutoExchange> autoExchange;

  if (hasPayout) {
    if (!parsePayoutMode(payoutMode, settingsPayout.Mode)) {
      replyWithStatus("invalid_payout_mode");
      return;
    }

    auto It = Server_.userManager().coinIdxMap().find(coin);
    if (It == Server_.userManager().coinIdxMap().end()) {
      replyWithStatus("invalid_coin");
      return;
    }

    CCoinInfo &coinInfo = Server_.userManager().coinInfo()[It->second];
    if (!parseMoneyValue(
          instantPayoutThreshold.c_str(),
          coinInfo.FractionalPartSize,
          &settingsPayout.InstantPayoutThreshold)) {
      replyWithStatus("request_format_error");
      return;
    }

    payout = std::move(settingsPayout);
  }

  if (hasMining) {
    CSettingsMining settingsMining;
    if (!parseMiningMode(miningMode, settingsMining.MiningMode)) {
      replyWithStatus("invalid_mining_mode");
      return;
    }

    mining = std::move(settingsMining);
  }

  if (hasAutoExchange)
    autoExchange = std::move(settingsAutoExchange);

  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, tokenInfo, true)) {
    replyWithStatus("unknown_id");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().updateSettings(
    tokenInfo.Login, coin,
    std::move(payout), std::move(mining), std::move(autoExchange),
    totp,
    [this](const char *status) {
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

static void jsonParseUserFeeConfig(rapidjson::Value &value, const char *fieldName, std::vector<UserFeePair> &config, bool *validAcc)
{
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
}

static void jsonParseModeFeeConfig(rapidjson::Value &document, CModeFeeConfig &modeCfg, bool *validAcc)
{
  jsonParseUserFeeConfig(document, "default", modeCfg.Default, validAcc);
  if (document.HasMember("coinSpecific") && document["coinSpecific"].IsArray()) {
    rapidjson::Value::Array coinArray = document["coinSpecific"].GetArray();
    for (rapidjson::SizeType i = 0, ie = coinArray.Size(); i != ie; ++i) {
      if (!coinArray[i].IsObject()) {
        *validAcc = false;
        break;
      }

      modeCfg.CoinSpecific.emplace_back();
      jsonParseString(coinArray[i], "coinName", modeCfg.CoinSpecific.back().CoinName, validAcc);
      jsonParseUserFeeConfig(coinArray[i], "config", modeCfg.CoinSpecific.back().Config, validAcc);
    }
  }
}

void PoolHttpConnection::onUserCreateFeePlan(rapidjson::Document &document)
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

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().createFeePlan(sessionId, feePlanId, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserUpdateFeePlan(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string feePlanId;
  std::string modeName;
  CModeFeeConfig modeConfig;

  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "feePlanId", feePlanId, &validAcc);
  jsonParseString(document, "mode", modeName, &validAcc);
  jsonParseModeFeeConfig(document, modeConfig, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  EMiningMode mode;
  if (!parseMiningMode(modeName, mode)) {
    replyWithStatus("invalid_mining_mode");
    return;
  }

  // Check coin
  for (const auto &cfg: modeConfig.CoinSpecific) {
    if (!Server_.backend(cfg.CoinName)) {
      replyWithStatus("invalid_coin");
      return;
    }
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().updateFeePlan(sessionId, feePlanId, mode, std::move(modeConfig), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserDeleteFeePlan(rapidjson::Document &document)
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

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().deleteFeePlan(sessionId, feePlanId, [this](const char *status) {
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
  std::vector<std::string> result;
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
        for (const auto &planId: result)
          plans.addString(planId);
      }
    }

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
  } else {
    replyWithStatus(status.c_str());
  }
}

void PoolHttpConnection::onUserQueryFeePlan(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string feePlanId;
  std::string modeName;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "feePlanId", feePlanId, &validAcc);
  jsonParseString(document, "mode", modeName, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  EMiningMode mode;
  if (!parseMiningMode(modeName, mode)) {
    replyWithStatus("invalid_mining_mode");
    return;
  }

  std::string status;
  CModeFeeConfig result;
  BaseBlob<256> referralId;
  if (Server_.userManager().queryFeePlan(sessionId, feePlanId, mode, status, referralId, result)) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    {
      JSON::Object answer(stream);
      answer.addString("status", "ok");
      answer.addString("feePlanId", feePlanId);
      answer.addString("mode", miningModeName(mode));
      if (!referralId.isNull())
        answer.addString("referralId", referralId.getHexRaw());
      else
        answer.addNull("referralId");
      addModeFeeConfigFields(stream, answer, result);
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

void PoolHttpConnection::onUserRenewFeePlanReferralId(rapidjson::Document &document)
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

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().renewFeePlanReferralId(sessionId, feePlanId, [this](const char *status, const std::string &referralId) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);
    {
      JSON::Object result(stream);
      result.addString("status", status);
      if (!referralId.empty())
        result.addString("referralId", referralId);
    }

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
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

void PoolHttpConnection::onUserAdjustInstantPayoutThreshold(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string coin;
  std::string threshold;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "coin", coin, &validAcc);
  jsonParseString(document, "threshold", threshold, &validAcc);
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

  UInt<384> minimalPayout;
  unsigned fractionalPartSize = backend->getCoinInfo().FractionalPartSize;
  if (!parseMoneyValue(threshold.c_str(), fractionalPartSize, &minimalPayout)) {
    replyWithStatus("request_format_error");
    return;
  }

  Server_.userManager().adjustInstantMinimalPayout(coin, minimalPayout);
  replyWithStatus("ok");
}

void PoolHttpConnection::onBackendManualPayout(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, &validAcc);
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
    backend->accountingDb()->queryUserBalance(tokenInfo.Login, [this, backend](const UserBalanceInfo &record) {
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

    AccountingDb::queryUserBalanceMulti(&accountingDbs[0], accountingDbs.size(), tokenInfo.Login, [this](const UserBalanceInfo *balanceData, size_t backendsNum) {
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

void PoolHttpConnection::onBackendQueryCoins(rapidjson::Document &document)
{
  // Parse optional session id for user-specific fee calculation
  bool validAcc = true;
  std::string sessionId;
  jsonParseString(document, "id", sessionId, "", &validAcc);

  std::string feePlanId = "default";
  UserManager::UserWithAccessRights tokenInfo;
  if (!sessionId.empty() && Server_.userManager().validateSession(sessionId, "", tokenInfo, false))
    feePlanId = Server_.userManager().getFeePlanId(tokenInfo.Login);

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
        auto backendCfg = backend->accountingDb()->backendSettings();
        object.addBoolean("ppsAvailable", backendCfg.PPSConfig.Enabled);

        // PPLNS fee: sum of user fee plan percentages
        double pplnsFee = 0.0;
        for (const auto &fee : Server_.userManager().getFeeRecord(feePlanId, EMiningMode::PPLNS, info.Name))
          pplnsFee += fee.Percentage;
        object.addDouble("pplnsFee", pplnsFee);

        // PPS fee: pool PPS fee + user fee plan
        double ppsFee = backendCfg.PPSConfig.PoolFee;
        for (const auto &fee : Server_.userManager().getFeeRecord(feePlanId, EMiningMode::PPS, info.Name))
          ppsFee += fee.Percentage;
        object.addDouble("ppsFee", ppsFee);

        // Minimal payouts
        object.addString("minimalRegularPayout",
                         FormatMoney(backendCfg.PayoutConfig.RegularMinimalPayout, info.FractionalPartSize));
        object.addString("minimalInstantPayout",
                         FormatMoney(backendCfg.PayoutConfig.InstantMinimalPayout, info.FractionalPartSize));

        // Swap flags
        object.addBoolean("acceptIncoming", backendCfg.SwapConfig.AcceptIncoming);
        object.addBoolean("acceptOutgoing", backendCfg.SwapConfig.AcceptOutgoing);

        // Current prices
        CPriceFetcher *priceFetcher = Server_.priceFetcher();
        if (priceFetcher) {
          double rateToBTC = priceFetcher->getPrice(info.Name);
          double btcToUSD = priceFetcher->getBtcUsd();
          if (rateToBTC > 0.0 && btcToUSD > 0.0) {
            object.addDouble("valueBTC", rateToBTC);
            object.addDouble("valueUSD", rateToBTC * btcToUSD);
          } else {
            object.addNull("valueBTC");
            object.addNull("valueUSD");
          }
        } else {
          object.addNull("valueBTC");
          object.addNull("valueUSD");
        }

        // Placeholders for future calculator fields
        object.addNull("height");
        object.addNull("difficulty");
        object.addNull("powerUnit");
        object.addNull("powerMultLog10");
        object.addNull("powerPPS");
        object.addNull("dailyPPS");
        object.addNull("dailyPPSUSD");
        object.addNull("dailyPPSBTC");
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
            block.addString("generatedCoins", FormatMoney(blocks[i].GeneratedCoins, coinInfo.FractionalPartSize));
            block.addString("foundBy", blocks[i].FoundBy);
            if (!blocks[i].MergedBlocks.empty()) {
              block.addField("mergedBlocks");
              JSON::Array mergedArray(stream);
              for (const auto &merged : blocks[i].MergedBlocks) {
                mergedArray.addField();
                JSON::Object mergedObj(stream);
                mergedObj.addString("coin", merged.CoinName);
                mergedObj.addInt("height", merged.Height);
                mergedObj.addString("hash", merged.Hash);
              }
            }
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
  int64_t timeFrom = 0;
  unsigned count;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, "", &validAcc);
  jsonParseInt64(document, "timeFrom", &timeFrom, 0, &validAcc);
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
  backend->queryPayouts(tokenInfo.Login, Timestamp(timeFrom), count, records);
  unsigned fractionalPartSize = backend->getCoinInfo().FractionalPartSize;
  double rateScale = std::pow(10.0, 8 - static_cast<int>(fractionalPartSize));
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
          payout.addInt("time", records[i].Time.count());
          payout.addString("txid", records[i].TransactionId);
          payout.addString("value", FormatMoney(records[i].Value, fractionalPartSize));
          UInt<384> valueBTC = records[i].Value;
          valueBTC.mulfp(records[i].RateToBTC * rateScale);
          UInt<384> valueUSD = records[i].Value;
          valueUSD.mulfp(records[i].RateToBTC * records[i].RateBTCToUSD * rateScale);
          payout.addString("valueBTC", FormatMoney(valueBTC, 8));
          payout.addString("valueUSD", FormatMoney(valueUSD, 8));
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
  backend->accountingDb()->queryPPLNSPayouts(tokenInfo.Login, timeFrom, hashFrom, count, [this, backend](const std::vector<CPPLNSPayoutInfo>& result) {
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
            payoutObject.addString("value", FormatMoney(payout.Value, backend->getCoinInfo().FractionalPartSize));
            payoutObject.addString("valueBTC", FormatMoney(payout.ValueBTC, 8));
            payoutObject.addString("valueUSD", FormatMoney(payout.ValueUSD, 8));
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
  backend->accountingDb()->queryPPLNSAcc(tokenInfo.Login, timeFrom, timeTo, groupByInterval, [this, backend](const std::vector<CPPLNSPayoutAcc>& result) {
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
          }
        }
      }
    }
    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryPPSPayouts(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  int64_t timeFrom;
  uint32_t count;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, &validAcc);
  jsonParseInt64(document, "timeFrom", &timeFrom, 0, &validAcc);
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

  auto result = backend->accountingDb()->api().queryPPSPayouts(tokenInfo.Login, timeFrom, count);

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
          payoutObject.addInt("startTime", payout.IntervalBegin.toUnixTime());
          payoutObject.addInt("endTime", payout.IntervalEnd.toUnixTime());
          payoutObject.addString("value", FormatMoney(payout.Value, backend->getCoinInfo().FractionalPartSize));
          payoutObject.addString("valueBTC", FormatMoney(payout.ValueBTC, 8));
          payoutObject.addString("valueUSD", FormatMoney(payout.ValueUSD, 8));
        }
      }
    }
  }
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onBackendQueryPPSPayoutsAcc(rapidjson::Document &document)
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

  if (timeTo <= timeFrom ||
      groupByInterval == 0 ||
      (timeTo - timeFrom) % groupByInterval != 0 ||
      (timeTo - timeFrom) / groupByInterval > 3200) {
    replyWithStatus("invalid_interval");
    return;
  }

  auto result = backend->accountingDb()->api().queryPPSPayoutsAcc(tokenInfo.Login, timeFrom, timeTo, groupByInterval);

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
        }
      }
    }
  }
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
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

void PoolHttpConnection::onBackendGetConfig(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string coin;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "coin", coin, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, "", tokenInfo, false) ||
      (tokenInfo.Login != "admin" && tokenInfo.Login != "observer")) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  CBackendSettings settings = backend->accountingDb()->backendSettings();
  unsigned fractionalPartSize = backend->getCoinInfo().FractionalPartSize;

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  {
    JSON::Object response(stream);
    response.addString("status", "ok");
    response.addField("pps");
    {
      JSON::Object pps(stream);
      pps.addBoolean("enabled", settings.PPSConfig.Enabled);
      pps.addDouble("poolFee", settings.PPSConfig.PoolFee);
      pps.addString(
        "saturationFunction",
        CBackendSettings::PPS::saturationFunctionName(settings.PPSConfig.SaturationFunction));
      pps.addDouble("saturationB0", settings.PPSConfig.SaturationB0);
      pps.addDouble("saturationANegative", settings.PPSConfig.SaturationANegative);
      pps.addDouble("saturationAPositive", settings.PPSConfig.SaturationAPositive);
      pps.addField("saturationFunctions");
      {
        JSON::Array arr(stream);
        for (const char *name : CBackendSettings::PPS::saturationFunctionNames())
          arr.addString(name);
      }
    }
    response.addField("payouts");
    {
      JSON::Object payouts(stream);
      payouts.addBoolean("instantPayoutsEnabled", settings.PayoutConfig.InstantPayoutsEnabled);
      payouts.addBoolean("regularPayoutsEnabled", settings.PayoutConfig.RegularPayoutsEnabled);
      payouts.addString("instantMinimalPayout", FormatMoney(settings.PayoutConfig.InstantMinimalPayout, fractionalPartSize));
      payouts.addInt("instantPayoutInterval", settings.PayoutConfig.InstantPayoutInterval.count());
      payouts.addString("regularMinimalPayout", FormatMoney(settings.PayoutConfig.RegularMinimalPayout, fractionalPartSize));
      payouts.addInt("regularPayoutInterval", settings.PayoutConfig.RegularPayoutInterval.count());
      payouts.addInt("regularPayoutDayOffset", settings.PayoutConfig.RegularPayoutDayOffset.count());
    }
    response.addField("swap");
    {
      JSON::Object swap(stream);
      swap.addBoolean("acceptIncoming", settings.SwapConfig.AcceptIncoming);
      swap.addBoolean("acceptOutgoing", settings.SwapConfig.AcceptOutgoing);
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

static void serializePPSState(xmstream &stream, const CPPSState &pps, unsigned fractionalPartSize)
{
  const auto &reward = pps.LastBaseBlockReward;
  JSON::Object obj(stream);
  obj.addInt("time", pps.Time.toUnixTime());
  obj.addField("balance");
  {
    JSON::Object balanceObj(stream);
    balanceObj.addString("value", FormatMoney(pps.Balance, fractionalPartSize));
    balanceObj.addDouble("inBlocks", CPPSState::balanceInBlocks(pps.Balance, reward));
  }
  obj.addField("refBalance");
  {
    JSON::Object refObj(stream);
    refObj.addString("value", FormatMoney(pps.ReferenceBalance, fractionalPartSize));
    refObj.addDouble("inBlocks", CPPSState::balanceInBlocks(pps.ReferenceBalance, reward));
    refObj.addDouble("sqLambda", CPPSState::sqLambda(pps.ReferenceBalance, reward, pps.TotalBlocksFound));
  }
  obj.addField("minRefBalance");
  {
    JSON::Object minObj(stream);
    minObj.addInt("time", pps.Min.Time.toUnixTime());
    minObj.addString("value", FormatMoney(pps.Min.Balance, fractionalPartSize));
    minObj.addDouble("inBlocks", CPPSState::balanceInBlocks(pps.Min.Balance, reward));
    minObj.addDouble("sqLambda", CPPSState::sqLambda(pps.Min.Balance, reward, pps.Min.TotalBlocksFound));
  }
  obj.addField("maxRefBalance");
  {
    JSON::Object maxObj(stream);
    maxObj.addInt("time", pps.Max.Time.toUnixTime());
    maxObj.addString("value", FormatMoney(pps.Max.Balance, fractionalPartSize));
    maxObj.addDouble("inBlocks", CPPSState::balanceInBlocks(pps.Max.Balance, reward));
    maxObj.addDouble("sqLambda", CPPSState::sqLambda(pps.Max.Balance, reward, pps.Max.TotalBlocksFound));
  }
  obj.addDouble("totalBlocksFound", pps.TotalBlocksFound);
  obj.addDouble("orphanBlocks", pps.OrphanBlocks);
  obj.addDouble("lastSaturateCoeff", pps.LastSaturateCoeff);
  obj.addString("lastBaseBlockReward", FormatMoney(pps.LastBaseBlockReward, fractionalPartSize));
  obj.addString("lastAverageTxFee", FormatMoney(pps.LastAverageTxFee, fractionalPartSize));
}

void PoolHttpConnection::onBackendGetPPSState(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string coin;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "coin", coin, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, "", tokenInfo, false) ||
      (tokenInfo.Login != "admin" && tokenInfo.Login != "observer")) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  unsigned fractionalPartSize = backend->getCoinInfo().FractionalPartSize;
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend->accountingDb()->queryPPSState([this, fractionalPartSize](const CPPSState &pps) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);
    {
      JSON::Object response(stream);
      response.addString("status", "ok");
      response.addField("state");
      serializePPSState(stream, pps, fractionalPartSize);
    }
    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryPPSHistory(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string coin;
  int64_t timeFrom;
  int64_t timeTo;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "coin", coin, &validAcc);
  jsonParseInt64(document, "timeFrom", &timeFrom, &validAcc);
  jsonParseInt64(document, "timeTo", &timeTo, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  UserManager::UserWithAccessRights tokenInfo;
  if (!Server_.userManager().validateSession(sessionId, "", tokenInfo, false) ||
      (tokenInfo.Login != "admin" && tokenInfo.Login != "observer")) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  unsigned fractionalPartSize = backend->getCoinInfo().FractionalPartSize;
  auto result = backend->accountingDb()->api().queryPPSHistory(timeFrom, timeTo);

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  {
    JSON::Object response(stream);
    response.addString("status", "ok");
    response.addField("history");
    {
      JSON::Array arr(stream);
      for (const auto &pps : result) {
        arr.addField();
        serializePPSState(stream, pps, fractionalPartSize);
      }
    }
  }
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onBackendUpdateConfig(rapidjson::Document &document)
{
  bool validAcc = true;
  std::string sessionId;
  std::string coin;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "coin", coin, &validAcc);

  bool hasPps = document.HasMember("pps");
  CBackendSettings::PPS ppsCfg;
  std::string saturationFunctionName;
  if (hasPps) {
    if (!document["pps"].IsObject()) {
      validAcc = false;
    } else {
      rapidjson::Value &ppsValue = document["pps"];
      jsonParseBoolean(ppsValue, "enabled", &ppsCfg.Enabled, &validAcc);
      jsonParseNumber(ppsValue, "poolFee", &ppsCfg.PoolFee, &validAcc);
      jsonParseString(ppsValue, "saturationFunction", saturationFunctionName, "", &validAcc);
      jsonParseNumber(ppsValue, "saturationB0", &ppsCfg.SaturationB0, 0.0, &validAcc);
      jsonParseNumber(ppsValue, "saturationANegative", &ppsCfg.SaturationANegative, 0.0, &validAcc);
      jsonParseNumber(ppsValue, "saturationAPositive", &ppsCfg.SaturationAPositive, 0.0, &validAcc);
    }
  }

  bool hasPayouts = document.HasMember("payouts");
  CBackendSettings::Payouts payoutsCfg;
  std::string instantMinimalPayout;
  std::string regularMinimalPayout;
  int64_t instantPayoutInterval = 0;
  int64_t regularPayoutInterval = 0;
  int64_t regularPayoutDayOffset = 0;
  if (hasPayouts) {
    if (!document["payouts"].IsObject()) {
      validAcc = false;
    } else {
      rapidjson::Value &payoutsValue = document["payouts"];
      jsonParseBoolean(payoutsValue, "instantPayoutsEnabled", &payoutsCfg.InstantPayoutsEnabled, &validAcc);
      jsonParseBoolean(payoutsValue, "regularPayoutsEnabled", &payoutsCfg.RegularPayoutsEnabled, &validAcc);
      jsonParseString(payoutsValue, "instantMinimalPayout", instantMinimalPayout, &validAcc);
      jsonParseInt64(payoutsValue, "instantPayoutInterval", &instantPayoutInterval, &validAcc);
      jsonParseString(payoutsValue, "regularMinimalPayout", regularMinimalPayout, &validAcc);
      jsonParseInt64(payoutsValue, "regularPayoutInterval", &regularPayoutInterval, &validAcc);
      jsonParseInt64(payoutsValue, "regularPayoutDayOffset", &regularPayoutDayOffset, 0, &validAcc);
    }
  }

  bool hasSwap = document.HasMember("swap");
  CBackendSettings::Swap swapCfg;
  if (hasSwap) {
    if (!document["swap"].IsObject()) {
      validAcc = false;
    } else {
      rapidjson::Value &swapValue = document["swap"];
      jsonParseBoolean(swapValue, "acceptIncoming", &swapCfg.AcceptIncoming, &validAcc);
      jsonParseBoolean(swapValue, "acceptOutgoing", &swapCfg.AcceptOutgoing, &validAcc);
    }
  }

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  if (!hasPps && !hasPayouts && !hasSwap) {
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

  std::optional<CBackendSettings::PPS> pps;
  std::optional<CBackendSettings::Payouts> payouts;
  std::optional<CBackendSettings::Swap> swap;

  if (hasPps) {
    if (!CBackendSettings::PPS::parseSaturationFunction(saturationFunctionName, &ppsCfg.SaturationFunction)) {
      replyWithStatus("invalid_saturation_function");
      return;
    }

    pps = std::move(ppsCfg);
  }

  if (hasPayouts) {
    unsigned fractionalPartSize = backend->getCoinInfo().FractionalPartSize;
    if (!parseMoneyValue(instantMinimalPayout.c_str(), fractionalPartSize, &payoutsCfg.InstantMinimalPayout) ||
        !parseMoneyValue(regularMinimalPayout.c_str(), fractionalPartSize, &payoutsCfg.RegularMinimalPayout)) {
      replyWithStatus("request_format_error");
      return;
    }

    payoutsCfg.InstantPayoutInterval = std::chrono::minutes(instantPayoutInterval);
    payoutsCfg.RegularPayoutInterval = std::chrono::hours(regularPayoutInterval);
    payoutsCfg.RegularPayoutDayOffset = std::chrono::hours(regularPayoutDayOffset);
    payouts = std::move(payoutsCfg);
  }

  if (hasSwap)
    swap = std::move(swapCfg);

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend->accountingDb()->updateBackendSettings(
    std::move(pps),
    std::move(payouts),
    std::move(swap),
    [this](const char *status) {
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
                               size_t threadsNum,
                               CPriceFetcher *priceFetcher) :
  Port_(port),
  UserMgr_(userMgr),
  MiningStats_(complexMiningStats),
  Config_(config),
  ThreadsNum_(threadsNum),
  PriceFetcher_(priceFetcher)
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
