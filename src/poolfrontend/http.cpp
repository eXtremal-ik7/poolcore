#include "http.h"
#include "poolcommon/utils.h"
#include "poolcore/priceFetcher.h"
#include "poolcore/thread.h"
#include "asyncio/coroutine.h"
#include "asyncio/socket.h"
#include "loguru.hpp"
#include "poolcommon/jsonSerializer.h"
#include <cmath>

std::unordered_map<std::string, PoolHttpConnection::FunctionInfo> PoolHttpConnection::FunctionNameMap_ = {
  // Public (no session)
  {"backendQueryCoins",                {fnBackendQueryCoins,                false, false, false}},
  {"backendPoolLuck",                  {fnBackendPoolLuck,                  false, false, false}},
  {"backendQueryFoundBlocks",          {fnBackendQueryFoundBlocks,          false, false, false}},
  {"backendQueryPoolBalance",          {fnBackendQueryPoolBalance,          false, false, false}},
  {"backendQueryPoolStats",            {fnBackendQueryPoolStats,            false, false, false}},
  {"backendQueryPoolStatsHistory",     {fnBackendQueryPoolStatsHistory,     false, false, false}},
  {"instanceEnumerateAll",             {fnInstanceEnumerateAll,             false, false, false}},
  {"userAction",                       {fnUserAction,                       false, false, false}},
  {"userChangeEmail",                  {fnUserChangeEmail,                  false, false, false}},
  {"userChangePasswordForce",          {fnUserChangePasswordForce,          false, false, false}},
  {"userChangePasswordInitiate",       {fnUserChangePasswordInitiate,       false, false, false}},
  {"userCreate",                       {fnUserCreate,                       false, false, false}},
  {"userLogin",                        {fnUserLogin,                        false, false, false}},
  {"userResendEmail",                  {fnUserResendEmail,                  false, false, false}},

  // Session, Read
  {"backendQueryPayouts",              {fnBackendQueryPayouts,              true,  false, false}},
  {"backendQueryPPLNSAcc",             {fnBackendQueryPPLNSAcc,             true,  false, false}},
  {"backendQueryPPLNSPayouts",         {fnBackendQueryPPLNSPayouts,         true,  false, false}},
  {"backendQueryPPSPayouts",           {fnBackendQueryPPSPayouts,           true,  false, false}},
  {"backendQueryPPSPayoutsAcc",        {fnBackendQueryPPSPayoutsAcc,        true,  false, false}},
  {"backendQueryUserBalance",          {fnBackendQueryUserBalance,          true,  false, false}},
  {"backendQueryUserStats",            {fnBackendQueryUserStats,            true,  false, false}},
  {"backendQueryUserStatsHistory",     {fnBackendQueryUserStatsHistory,     true,  false, false}},
  {"backendQueryWorkerStatsHistory",   {fnBackendQueryWorkerStatsHistory,   true,  false, false}},
  {"userEnumerateAll",                 {fnUserEnumerateAll,                 true,  false, false}},
  {"userEnumerateFeePlan",             {fnUserEnumerateFeePlan,             true,  false, false}},
  {"userGetCredentials",               {fnUserGetCredentials,               true,  false, false}},
  {"userGetSettings",                  {fnUserGetSettings,                  true,  false, false}},
  {"userLogout",                       {fnUserLogout,                       true,  false, false}},
  {"userQueryFeePlan",                 {fnUserQueryFeePlan,                 true,  false, false}},
  {"userQueryMonitoringSession",       {fnUserQueryMonitoringSession,       true,  false, false}},

  // Session, Write
  {"backendManualPayout",              {fnBackendManualPayout,              true,  true,  false}},
  {"userActivate2faInitiate",          {fnUserActivate2faInitiate,          true,  true,  false}},
  {"userDeactivate2faInitiate",        {fnUserDeactivate2faInitiate,        true,  true,  false}},
  {"userUpdateCredentials",            {fnUserUpdateCredentials,            true,  true,  false}},
  {"userUpdateSettings",               {fnUserUpdateSettings,               true,  true,  false}},

  // SuperUser, Read (admin + observer)
  {"backendGetConfig",                 {fnBackendGetConfig,                 true,  false, true}},
  {"backendGetPPSState",               {fnBackendGetPPSState,               true,  false, true}},
  {"backendQueryPPSHistory",           {fnBackendQueryPPSHistory,           true,  false, true}},
  {"backendQueryProfitSwitchCoeff",    {fnBackendQueryProfitSwitchCoeff,    true,  false, true}},
  {"complexMiningStatsGetInfo",        {fnComplexMiningStatsGetInfo,        true,  false, true}},

  // SuperUser, Write (admin only)
  {"backendUpdateConfig",              {fnBackendUpdateConfig,              true,  true,  true}},
  {"backendUpdateProfitSwitchCoeff",   {fnBackendUpdateProfitSwitchCoeff,   true,  true,  true}},
  {"userAdjustInstantPayoutThreshold", {fnUserAdjustInstantPayoutThreshold, true,  true,  true}},
  {"userChangeFeePlan",                {fnUserChangeFeePlan,                true,  true,  true}},
  {"userCreateFeePlan",                {fnUserCreateFeePlan,                true,  true,  true}},
  {"userCreateForce",                  {fnUserCreateForce,                  true,  true,  true}},
  {"userDeleteFeePlan",                {fnUserDeleteFeePlan,                true,  true,  true}},
  {"userRenewFeePlanReferralId",       {fnUserRenewFeePlanReferralId,       true,  true,  true}},
  {"userUpdateFeePlan",                {fnUserUpdateFeePlan,                true,  true,  true}},
};

static inline bool rawcmp(Raw data, const char *operand) {
  size_t opSize = strlen(operand);
  return data.size == opSize && memcmp(data.data, operand, opSize) == 0;
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
    Context.parseState = psUnknown;
    Context.Function = nullptr;
    return 1;
  }

  if (component->type == httpRequestDtUriPathElement) {
    if (Context.parseState == psUnknown && rawcmp(component->data, "api")) {
      Context.parseState = psApi;
    } else if (Context.parseState == psApi) {
      if (Context.method != hmPost) {
        reply404();
        return 0;
      }

      std::string functionName(component->data.data, component->data.data + component->data.size);
      auto It = FunctionNameMap_.find(functionName);
      if (It == FunctionNameMap_.end()) {
        reply404();
        return 0;
      }

      Context.Function = &It->second;
      Context.parseState = psResolved;
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

    if (!Context.Function) {
      reply404();
      return 0;
    }

    switch (Context.Function->Type) {
      // Plain
      case fnBackendQueryCoins: dispatch<&PoolHttpConnection::onBackendQueryCoins>(); break;
      case fnBackendQueryPoolBalance: onBackendQueryPoolBalance(); break;
      case fnBackendQueryPoolStats: dispatch<&PoolHttpConnection::onBackendQueryPoolStats>(); break;
      case fnInstanceEnumerateAll: onInstanceEnumerateAll(); break;
      case fnUserAction: dispatch<&PoolHttpConnection::onUserAction>(); break;
      case fnUserChangeEmail: onUserChangeEmail(); break;
      case fnUserChangePasswordForce: dispatch<&PoolHttpConnection::onUserChangePasswordForce>(); break;
      case fnUserChangePasswordInitiate: dispatch<&PoolHttpConnection::onUserChangePasswordInitiate>(); break;
      case fnUserCreate: dispatch<&PoolHttpConnection::onUserCreate>(); break;
      case fnUserLogin: dispatch<&PoolHttpConnection::onUserLogin>(); break;
      case fnUserResendEmail: dispatch<&PoolHttpConnection::onUserResendEmail>(); break;
      // Backend
      case fnBackendPoolLuck: dispatch<&PoolHttpConnection::onBackendPoolLuck>(); break;
      case fnBackendQueryFoundBlocks: dispatch<&PoolHttpConnection::onBackendQueryFoundBlocks>(); break;
      // Statistic
      case fnBackendQueryPoolStatsHistory: dispatch<&PoolHttpConnection::onBackendQueryPoolStatsHistory>(); break;
      // Session
      case fnBackendQueryProfitSwitchCoeff: dispatch<&PoolHttpConnection::onBackendQueryProfitSwitchCoeff>(); break;
      case fnBackendQueryUserBalance: dispatch<&PoolHttpConnection::onBackendQueryUserBalance>(); break;
      case fnComplexMiningStatsGetInfo: dispatch<&PoolHttpConnection::onComplexMiningStatsGetInfo>(); break;
      case fnUserActivate2faInitiate: dispatch<&PoolHttpConnection::onUserActivate2faInitiate>(); break;
      case fnUserChangeFeePlan: dispatch<&PoolHttpConnection::onUserChangeFeePlan>(); break;
      case fnUserCreateFeePlan: dispatch<&PoolHttpConnection::onUserCreateFeePlan>(); break;
      case fnUserCreateForce: dispatch<&PoolHttpConnection::onUserCreateForce>(); break;
      case fnUserDeactivate2faInitiate: dispatch<&PoolHttpConnection::onUserDeactivate2faInitiate>(); break;
      case fnUserDeleteFeePlan: dispatch<&PoolHttpConnection::onUserDeleteFeePlan>(); break;
      case fnUserEnumerateFeePlan: dispatch<&PoolHttpConnection::onUserEnumerateFeePlan>(); break;
      case fnUserGetCredentials: dispatch<&PoolHttpConnection::onUserGetCredentials>(); break;
      case fnUserGetSettings: dispatch<&PoolHttpConnection::onUserGetSettings>(); break;
      case fnUserLogout: dispatch<&PoolHttpConnection::onUserLogout>(); break;
      case fnUserQueryFeePlan: dispatch<&PoolHttpConnection::onUserQueryFeePlan>(); break;
      case fnUserQueryMonitoringSession: dispatch<&PoolHttpConnection::onUserQueryMonitoringSession>(); break;
      case fnUserRenewFeePlanReferralId: dispatch<&PoolHttpConnection::onUserRenewFeePlanReferralId>(); break;
      case fnUserUpdateCredentials: dispatch<&PoolHttpConnection::onUserUpdateCredentials>(); break;
      case fnUserUpdateFeePlan: dispatch<&PoolHttpConnection::onUserUpdateFeePlan>(); break;
      case fnUserUpdateSettings: dispatch<&PoolHttpConnection::onUserUpdateSettings>(); break;
      // Session + Statistic
      case fnBackendQueryUserStats: dispatch<&PoolHttpConnection::onBackendQueryUserStats>(); break;
      case fnBackendQueryUserStatsHistory: dispatch<&PoolHttpConnection::onBackendQueryUserStatsHistory>(); break;
      case fnBackendQueryWorkerStatsHistory: dispatch<&PoolHttpConnection::onBackendQueryWorkerStatsHistory>(); break;
      case fnUserEnumerateAll: dispatch<&PoolHttpConnection::onUserEnumerateAll>(); break;
      // Session + Backend
      case fnBackendGetConfig: dispatch<&PoolHttpConnection::onBackendGetConfig>(); break;
      case fnBackendGetPPSState: dispatch<&PoolHttpConnection::onBackendGetPPSState>(); break;
      case fnBackendManualPayout: dispatch<&PoolHttpConnection::onBackendManualPayout>(); break;
      case fnBackendQueryPayouts: dispatch<&PoolHttpConnection::onBackendQueryPayouts>(); break;
      case fnBackendQueryPPLNSAcc: dispatch<&PoolHttpConnection::onBackendQueryPPLNSAcc>(); break;
      case fnBackendQueryPPLNSPayouts: dispatch<&PoolHttpConnection::onBackendQueryPPLNSPayouts>(); break;
      case fnBackendQueryPPSHistory: dispatch<&PoolHttpConnection::onBackendQueryPPSHistory>(); break;
      case fnBackendQueryPPSPayouts: dispatch<&PoolHttpConnection::onBackendQueryPPSPayouts>(); break;
      case fnBackendQueryPPSPayoutsAcc: dispatch<&PoolHttpConnection::onBackendQueryPPSPayoutsAcc>(); break;
      case fnBackendUpdateConfig: dispatch<&PoolHttpConnection::onBackendUpdateConfig>(); break;
      case fnBackendUpdateProfitSwitchCoeff: dispatch<&PoolHttpConnection::onBackendUpdateProfitSwitchCoeff>(); break;
      case fnUserAdjustInstantPayoutThreshold: dispatch<&PoolHttpConnection::onUserAdjustInstantPayoutThreshold>(); break;
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

UserManager &PoolHttpConnection::userManager() { return Server_.userManager(); }
PoolBackend *PoolHttpConnection::backend(const std::string &coin) { return Server_.backend(coin); }
StatisticDb *PoolHttpConnection::statistic(const std::string &coin) { return Server_.statisticDb(coin); }

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

template<typename T>
static UserManager::Credentials credentialsFromRequest(const T &req) {
  UserManager::Credentials c;
  c.Login = req.Login;
  c.Password = req.Password;
  c.Name = req.Name;
  c.EMail = req.Email;
  c.TwoFactor = req.Totp;
  c.IsActive = req.IsActive;
  c.IsReadOnly = req.IsReadOnly;
  c.FeePlan = req.FeePlanId;
  c.ReferralId = req.ReferralId;
  return c;
}

void PoolHttpConnection::onUserAction(const CUserActionRequest &request)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userAction(request.ActionId, request.NewPassword, request.Totp, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserCreate(const CUserCreateRequest &request)
{
  UserManager::Credentials credentials;
  credentials.Login = request.Login;
  credentials.Password = request.Password;
  credentials.Name = request.Name;
  credentials.EMail = request.Email;
  credentials.TwoFactor = request.Totp;
  credentials.ReferralId = request.ReferralId;

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userCreate("", std::move(credentials), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserCreateForce(const CUserCreateForceRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  UserManager::Credentials credentials = credentialsFromRequest(request);

  if (!credentials.FeePlan.empty() && !credentials.ReferralId.empty()) {
    replyWithStatus("request_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userCreate(tokenInfo.Login, std::move(credentials), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserResendEmail(const CUserResendEmailRequest &request)
{
  UserManager::Credentials credentials = credentialsFromRequest(request);

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userResendEmail(std::move(credentials), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserLogin(const CUserLoginRequest &request)
{
  UserManager::Credentials credentials = credentialsFromRequest(request);

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

void PoolHttpConnection::onUserLogout(const CUserLogoutRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userLogout(request.Id, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserQueryMonitoringSession(const CUserQueryMonitoringSessionRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userQueryMonitoringSession(tokenInfo.Login, [this](const std::string &sessionId, const char *status) {
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

void PoolHttpConnection::onUserChangeEmail()
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserChangePasswordInitiate(const CUserChangePasswordInitiateRequest &request)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userChangePasswordInitiate(request.Login, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserChangePasswordForce(const CUserChangePasswordForceRequest &request)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userChangePasswordForce(request.Id, request.Login, request.NewPassword, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserGetCredentials(const CUserGetCredentialsRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object result(stream);
    UserManager::Credentials credentials;
    if (Server_.userManager().getUserCredentials(tokenInfo.Login, credentials)) {
      result.addString("status", "ok");
      result.addString("login", tokenInfo.Login);
      result.addString("name", credentials.Name);
      result.addString("email", credentials.EMail);
      result.addInt("registrationDate", credentials.RegistrationDate.toUnixTime());
      result.addBoolean("isActive", credentials.IsActive);
      // We return readonly flag if user or session has it
      result.addBoolean("isReadOnly", tokenInfo.IsReadOnly | credentials.IsReadOnly);
      result.addBoolean("has2fa", credentials.HasTwoFactor);
    } else {
      result.addString("status", "unknown_id");
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserGetSettings(const CUserGetSettingsRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  std::string feePlanId = Server_.userManager().getFeePlanId(tokenInfo.Login);

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object object(stream);
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
        payout.addString("mode", EPayoutModeToString(settings.Payout.Mode));
        payout.addString("address", settings.Payout.Address);
        payout.addString("instantPayoutThreshold", FormatMoney(settings.Payout.InstantPayoutThreshold, coinInfo.FractionalPartSize));
      }
      coin.addField("mining");
      {
        JSON::Object mining(stream);
        mining.addString("mode", EMiningModeToString(settings.Mining.MiningMode));
      }
      coin.addField("autoExchange");
      {
        JSON::Object autoExchange(stream);
        autoExchange.addString("payoutCoinName", settings.AutoExchange.PayoutCoinName);
      }

      // Per-user fee
      PoolBackend *backend = Server_.backend(coinInfo.Name);
      double pplnsFee = 0.0;
      for (const auto &fee : Server_.userManager().getFeeRecord(feePlanId, EMiningMode::Pplns, coinInfo.Name))
        pplnsFee += fee.Percentage;
      coin.addDouble("pplnsFee", pplnsFee);

      double ppsFee = backend ? backend->accountingDb()->backendSettings().PPSConfig.PoolFee : 0.0;
      for (const auto &fee : Server_.userManager().getFeeRecord(feePlanId, EMiningMode::Pps, coinInfo.Name))
        ppsFee += fee.Percentage;
      coin.addDouble("ppsFee", ppsFee);
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserUpdateCredentials(const CUserUpdateCredentialsRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  UserManager::Credentials credentials = credentialsFromRequest(request);

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().updateCredentials(tokenInfo.Login, std::move(credentials), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserUpdateSettings(const CUserUpdateSettingsRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  if (!request.Payout.has_value() && !request.Mining.has_value() && !request.AutoExchange.has_value()) {
    replyWithStatus("json_format_error");
    return;
  }

  std::optional<CSettingsPayout> payout;
  std::optional<CSettingsMining> mining;
  std::optional<CSettingsAutoExchange> autoExchange;

  if (request.Payout.has_value()) {
    CSettingsPayout settingsPayout;
    settingsPayout.Mode = request.Payout->Mode;
    settingsPayout.Address = request.Payout->Address;

    auto It = Server_.userManager().coinIdxMap().find(request.Coin);
    if (It == Server_.userManager().coinIdxMap().end()) {
      replyWithStatus("invalid_coin");
      return;
    }

    CCoinInfo &coinInfo = Server_.userManager().coinInfo()[It->second];
    if (!parseMoneyValue(
          request.Payout->InstantPayoutThreshold.c_str(),
          coinInfo.FractionalPartSize,
          &settingsPayout.InstantPayoutThreshold)) {
      replyWithStatus("request_format_error");
      return;
    }

    payout = std::move(settingsPayout);
  }

  if (request.Mining.has_value()) {
    CSettingsMining settingsMining;
    settingsMining.MiningMode = request.Mining->Mode;
    mining = std::move(settingsMining);
  }

  if (request.AutoExchange.has_value()) {
    CSettingsAutoExchange settingsAutoExchange;
    settingsAutoExchange.PayoutCoinName = request.AutoExchange->PayoutCoinName;
    autoExchange = std::move(settingsAutoExchange);
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().updateSettings(
    tokenInfo.Login, request.Coin,
    std::move(payout), std::move(mining), std::move(autoExchange),
    request.Totp,
    [this](const char *status) {
      replyWithStatus(status);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
}

void PoolHttpConnection::onUserEnumerateAll(const CUserEnumerateAllRequest &request, const UserManager::UserWithAccessRights &tokenInfo, StatisticDb &statistic)
{
  StatisticDb::CredentialsWithStatistic::EColumns column;
  switch (request.SortBy) {
    case EUserSortColumn::Login: column = StatisticDb::CredentialsWithStatistic::ELogin; break;
    case EUserSortColumn::WorkersNum: column = StatisticDb::CredentialsWithStatistic::EWorkersNum; break;
    case EUserSortColumn::AveragePower: column = StatisticDb::CredentialsWithStatistic::EAveragePower; break;
    case EUserSortColumn::SharesPerSecond: column = StatisticDb::CredentialsWithStatistic::ESharesPerSecond; break;
    case EUserSortColumn::LastShareTime: column = StatisticDb::CredentialsWithStatistic::ELastShareTime; break;
  }

  uint64_t offset = request.Offset;
  uint64_t size = request.Size;
  bool sortDescending = request.SortDescending;

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().enumerateUsers(tokenInfo.Login, [this, &statistic, offset, size, column, sortDescending](const char *status, std::vector<UserManager::Credentials> &allUsers) {
    statistic.queryAllusersStats(std::move(allUsers), [this, status](const std::vector<StatisticDb::CredentialsWithStatistic> &result) {
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
              userObject.addInt("registrationDate", user.Credentials.RegistrationDate.toUnixTime());
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


void PoolHttpConnection::onUserCreateFeePlan(const CUserCreateFeePlanRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().createFeePlan(request.FeePlanId, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserUpdateFeePlan(const CUserUpdateFeePlanRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  EMiningMode mode = request.Mode;

  CModeFeeConfig modeConfig;
  for (const auto &p : request.Default)
    modeConfig.Default.push_back({p.UserId, p.Percentage});
  for (const auto &c : request.CoinSpecific) {
    CUserFeeConfig ucfg;
    ucfg.CoinName = c.CoinName;
    for (const auto &p : c.Config)
      ucfg.Config.push_back({p.UserId, p.Percentage});
    modeConfig.CoinSpecific.push_back(std::move(ucfg));
  }

  // Check coin
  for (const auto &cfg: modeConfig.CoinSpecific) {
    if (!Server_.backend(cfg.CoinName)) {
      replyWithStatus("invalid_coin");
      return;
    }
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().updateFeePlan(request.FeePlanId, mode, std::move(modeConfig), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserDeleteFeePlan(const CUserDeleteFeePlanRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().deleteFeePlan(request.FeePlanId, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserEnumerateFeePlan(const CUserEnumerateFeePlanRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  std::string status;
  std::vector<std::string> result;
  if (Server_.userManager().enumerateFeePlan(tokenInfo.Login, status, result)) {
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

void PoolHttpConnection::onUserQueryFeePlan(const CUserQueryFeePlanRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  EMiningMode mode = request.Mode;

  std::string status;
  CModeFeeConfig result;
  BaseBlob<256> referralId;
  if (Server_.userManager().queryFeePlan(tokenInfo.Login, request.FeePlanId, mode, status, referralId, result)) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    {
      JSON::Object answer(stream);
      answer.addString("status", "ok");
      answer.addString("feePlanId", request.FeePlanId);
      answer.addString("mode", EMiningModeToString(mode));
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

void PoolHttpConnection::onUserChangeFeePlan(const CUserChangeFeePlanRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().changeFeePlan(tokenInfo.Login, request.FeePlanId, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserRenewFeePlanReferralId(const CUserRenewFeePlanReferralIdRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().renewFeePlanReferralId(request.FeePlanId, [this](const char *status, const std::string &referralId) {
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

void PoolHttpConnection::onUserActivate2faInitiate(const CUserActivate2faInitiateRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().activate2faInitiate(tokenInfo.Login, [this](const char *status, const char *key) {
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

void PoolHttpConnection::onUserDeactivate2faInitiate(const CUserDeactivate2faInitiateRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().deactivate2faInitiate(tokenInfo.Login, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserAdjustInstantPayoutThreshold(const CUserAdjustInstantPayoutThresholdRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend)
{
  UInt<384> minimalPayout;
  unsigned fractionalPartSize = backend.getCoinInfo().FractionalPartSize;
  if (!parseMoneyValue(request.Threshold.c_str(), fractionalPartSize, &minimalPayout)) {
    replyWithStatus("request_format_error");
    return;
  }

  Server_.userManager().adjustInstantMinimalPayout(request.Coin, minimalPayout);
  replyWithStatus("ok");
}

void PoolHttpConnection::onBackendManualPayout(const CBackendManualPayoutRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend.accountingDb()->manualPayout(tokenInfo.Login, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryUserBalance(const CBackendQueryUserBalanceRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  if (!request.Coin.empty()) {
    PoolBackend *backend = Server_.backend(request.Coin);
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

void PoolHttpConnection::onBackendQueryUserStats(const CBackendQueryUserStatsRequest &request, const UserManager::UserWithAccessRights &tokenInfo, StatisticDb &statistic)
{
  StatisticDb::EStatsColumn column;
  switch (request.SortBy) {
    case EWorkerSortColumn::Name: column = StatisticDb::EStatsColumnName; break;
    case EWorkerSortColumn::AveragePower: column = StatisticDb::EStatsColumnAveragePower; break;
    case EWorkerSortColumn::SharesPerSecond: column = StatisticDb::EStatsColumnSharesPerSecond; break;
    case EWorkerSortColumn::LastShareTime: column = StatisticDb::EStatsColumnLastShareTime; break;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  statistic.queryUserStats(tokenInfo.Login, [this, &statistic](const CStats &aggregate, const std::vector<CStats> &workers) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    {
      JSON::Object object(stream);
      object.addString("status", "ok");
      object.addString("powerUnit", statistic.getCoinInfo().getPowerUnitName());
      object.addInt("powerMultLog10", statistic.getCoinInfo().PowerMultLog10);
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
  }, request.Offset, request.Size, column, request.SortDescending);
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

void PoolHttpConnection::onBackendQueryUserStatsHistory(const CBackendQueryUserStatsHistoryRequest &request, const UserManager::UserWithAccessRights &tokenInfo, StatisticDb &statistic)
{
  queryStatsHistory(&statistic, tokenInfo.Login, "", request.TimeFrom, request.TimeTo, request.GroupByInterval, time(nullptr));
}

void PoolHttpConnection::onBackendQueryWorkerStatsHistory(const CBackendQueryWorkerStatsHistoryRequest &request, const UserManager::UserWithAccessRights &tokenInfo, StatisticDb &statistic)
{
  queryStatsHistory(&statistic, tokenInfo.Login, request.WorkerId, request.TimeFrom, request.TimeTo, request.GroupByInterval, time(nullptr));
}

void PoolHttpConnection::onBackendQueryCoins(const CBackendQueryCoinsRequest &request)
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
        auto backendCfg = backend->accountingDb()->backendSettings();
        object.addBoolean("ppsAvailable", backendCfg.PPSConfig.Enabled);

        // PPLNS fee: sum of default fee plan percentages
        double pplnsFee = 0.0;
        for (const auto &fee : Server_.userManager().getFeeRecord("default", EMiningMode::Pplns, info.Name))
          pplnsFee += fee.Percentage;
        object.addDouble("pplnsFee", pplnsFee);

        // PPS fee: pool PPS fee + default fee plan
        double ppsFee = backendCfg.PPSConfig.PoolFee;
        for (const auto &fee : Server_.userManager().getFeeRecord("default", EMiningMode::Pps, info.Name))
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

void PoolHttpConnection::onBackendQueryFoundBlocks(const CBackendQueryFoundBlocksRequest &request, PoolBackend &backend)
{
  const CCoinInfo &coinInfo = backend.getCoinInfo();

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend.accountingDb()->queryFoundBlocks(request.HeightFrom, request.HashFrom, request.Count, [this, &coinInfo](const std::vector<FoundBlockRecord> &blocks, const std::vector<CNetworkClient::GetBlockConfirmationsQuery> &confirmations) {
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

void PoolHttpConnection::onBackendQueryPayouts(const CBackendQueryPayoutsRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend)
{
  std::vector<PayoutDbRecord> records;
  backend.queryPayouts(tokenInfo.Login, Timestamp(request.TimeFrom), request.Count, records);
  unsigned fractionalPartSize = backend.getCoinInfo().FractionalPartSize;
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

void PoolHttpConnection::onBackendQueryPoolBalance()
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onBackendQueryPoolStats(const CBackendQueryPoolStatsRequest &request)
{
  if (!request.Coin.empty()) {
    StatisticDb *statistic = Server_.statisticDb(request.Coin);
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

void PoolHttpConnection::onBackendQueryPoolStatsHistory(const CBackendQueryPoolStatsHistoryRequest &request, StatisticDb &statistic)
{
  queryStatsHistory(&statistic, "", "", request.TimeFrom, request.TimeTo, request.GroupByInterval, time(nullptr));
}

void PoolHttpConnection::onBackendQueryProfitSwitchCoeff(const CBackendQueryProfitSwitchCoeffRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
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

void PoolHttpConnection::onBackendQueryPPLNSPayouts(const CBackendQueryPPLNSPayoutsRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend.accountingDb()->queryPPLNSPayouts(tokenInfo.Login, request.TimeFrom, request.HashFrom, request.Count, [this, &backend](const std::vector<CPPLNSPayoutInfo>& result) {
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
            payoutObject.addString("value", FormatMoney(payout.Value, backend.getCoinInfo().FractionalPartSize));
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

void PoolHttpConnection::onBackendQueryPPLNSAcc(const CBackendQueryPPLNSAccRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend)
{
  if (request.TimeTo <= request.TimeFrom ||
      request.GroupByInterval == 0 ||
      (request.TimeTo - request.TimeFrom) % request.GroupByInterval != 0 ||
      (request.TimeTo - request.TimeFrom) / request.GroupByInterval > 3200) {
    replyWithStatus("invalid_interval");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend.accountingDb()->queryPPLNSAcc(tokenInfo.Login, request.TimeFrom, request.TimeTo, request.GroupByInterval, [this, &backend](const std::vector<CPPLNSPayoutAcc>& result) {
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
            payoutObject.addString("value", FormatMoney(payout.TotalCoin, backend.getCoinInfo().FractionalPartSize));
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

void PoolHttpConnection::onBackendQueryPPSPayouts(const CBackendQueryPPSPayoutsRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend)
{
  auto result = backend.accountingDb()->api().queryPPSPayouts(tokenInfo.Login, request.TimeFrom, request.Count);

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
          payoutObject.addString("value", FormatMoney(payout.Value, backend.getCoinInfo().FractionalPartSize));
          payoutObject.addString("valueBTC", FormatMoney(payout.ValueBTC, 8));
          payoutObject.addString("valueUSD", FormatMoney(payout.ValueUSD, 8));
        }
      }
    }
  }
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onBackendQueryPPSPayoutsAcc(const CBackendQueryPPSPayoutsAccRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend)
{
  if (request.TimeTo <= request.TimeFrom ||
      request.GroupByInterval == 0 ||
      (request.TimeTo - request.TimeFrom) % request.GroupByInterval != 0 ||
      (request.TimeTo - request.TimeFrom) / request.GroupByInterval > 3200) {
    replyWithStatus("invalid_interval");
    return;
  }

  auto result = backend.accountingDb()->api().queryPPSPayoutsAcc(tokenInfo.Login, request.TimeFrom, request.TimeTo, request.GroupByInterval);

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
          payoutObject.addString("value", FormatMoney(payout.TotalCoin, backend.getCoinInfo().FractionalPartSize));
          payoutObject.addString("valueBTC", FormatMoney(payout.TotalBTC, 8));
          payoutObject.addString("valueUSD", FormatMoney(payout.TotalUSD, 8));
        }
      }
    }
  }
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onBackendUpdateProfitSwitchCoeff(const CBackendUpdateProfitSwitchCoeffRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend)
{
  backend.setProfitSwitchCoeff(request.ProfitSwitchCoeff);
  replyWithStatus("ok");
}

void PoolHttpConnection::onBackendGetConfig(const CBackendGetConfigRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend)
{
  CBackendSettings settings = backend.accountingDb()->backendSettings();
  unsigned fractionalPartSize = backend.getCoinInfo().FractionalPartSize;

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
        ESaturationFunctionToString(settings.PPSConfig.SaturationFunction));
      pps.addDouble("saturationB0", settings.PPSConfig.SaturationB0);
      pps.addDouble("saturationANegative", settings.PPSConfig.SaturationANegative);
      pps.addDouble("saturationAPositive", settings.PPSConfig.SaturationAPositive);
      pps.addField("saturationFunctions");
      {
        JSON::Array arr(stream);
        for (const char *name : ESaturationFunctionNames())
          arr.addString(name);
      }
    }
    response.addField("payouts");
    stream.write(settings.PayoutConfig.serialize([fractionalPartSize](const money_t &value) -> std::string {
      return FormatMoney(value, fractionalPartSize);
    }));
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

void PoolHttpConnection::onBackendGetPPSState(const CBackendGetPPSStateRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend)
{
  unsigned fractionalPartSize = backend.getCoinInfo().FractionalPartSize;
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend.accountingDb()->queryPPSState([this, fractionalPartSize](const CPPSState &pps) {
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

void PoolHttpConnection::onBackendQueryPPSHistory(const CBackendQueryPPSHistoryRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend)
{
  unsigned fractionalPartSize = backend.getCoinInfo().FractionalPartSize;
  auto result = backend.accountingDb()->api().queryPPSHistory(request.TimeFrom, request.TimeTo);

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

void PoolHttpConnection::onBackendUpdateConfig(const CBackendUpdateConfigRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend)
{
  // Re-parse with resolver to convert money values using coin-specific fractional part size
  CBackendUpdateConfigRequest resolvedRequest;
  unsigned fractionalPartSize = backend.getCoinInfo().FractionalPartSize;
  if (!resolvedRequest.parse(Context.Request.c_str(), Context.Request.size(),
    [fractionalPartSize](const std::string &, const std::string &raw, money_t &value) -> bool {
      return parseMoneyValue(raw.c_str(), fractionalPartSize, &value);
    })) {
    replyWithStatus("request_format_error");
    return;
  }

  if (!resolvedRequest.Pps.has_value() && !resolvedRequest.Payouts.has_value() && !resolvedRequest.Swap.has_value()) {
    replyWithStatus("json_format_error");
    return;
  }

  std::optional<CBackendPPS> pps;
  if (resolvedRequest.Pps.has_value()) {
    CBackendPPS ppsCfg;
    ppsCfg.Enabled = resolvedRequest.Pps->Enabled;
    ppsCfg.PoolFee = resolvedRequest.Pps->PoolFee;
    ppsCfg.SaturationFunction = resolvedRequest.Pps->SaturationFunction;
    ppsCfg.SaturationB0 = resolvedRequest.Pps->SaturationB0;
    ppsCfg.SaturationANegative = resolvedRequest.Pps->SaturationANegative;
    ppsCfg.SaturationAPositive = resolvedRequest.Pps->SaturationAPositive;
    pps = std::move(ppsCfg);
  }

  std::optional<CBackendSwap> swap;
  if (resolvedRequest.Swap.has_value()) {
    CBackendSwap swapCfg;
    swapCfg.AcceptIncoming = resolvedRequest.Swap->AcceptIncoming;
    swapCfg.AcceptOutgoing = resolvedRequest.Swap->AcceptOutgoing;
    swap = std::move(swapCfg);
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend.accountingDb()->updateBackendSettings(
    std::move(pps),
    std::move(resolvedRequest.Payouts),
    std::move(swap),
    [this](const char *status) {
      replyWithStatus(status);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
}

void PoolHttpConnection::onBackendPoolLuck(const CBackendPoolLuckRequest &request, PoolBackend &backend)
{
  // Validate ascending order
  for (size_t i = 1; i < request.Intervals.size(); i++) {
    if (request.Intervals[i] <= request.Intervals[i-1]) {
      replyWithStatus("json_format_error");
      return;
    }
  }

  std::vector<int64_t> intervals = request.Intervals;
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend.accountingDb()->poolLuck(std::move(intervals), [this](const std::vector<double> &result) {
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

void PoolHttpConnection::onInstanceEnumerateAll()
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
        instanceObject.addDouble("shareDiff", instance.ShareDiff);
      }
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onComplexMiningStatsGetInfo(const CComplexMiningStatsGetInfoRequest &request, const UserManager::UserWithAccessRights &tokenInfo)
{
  const char *data = Context.Request.c_str();
  size_t size = Context.Request.size();
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.miningStats().query(data, size, [this](const char *data, size_t size) {
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
    CLOG_F(ERROR, "PoolHttpServer: can't bind port {}", static_cast<unsigned>(Port_));
    return false;
  }

  if (socketListen(hSocket) != 0) {
    CLOG_F(ERROR, "PoolHttpServer: can't listen port {}", static_cast<unsigned>(Port_));
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
      CLOG_F(INFO, "http server started tid={}", GetGlobalThreadId());
      asyncLoop(server->Base_);
    }, this);
  }

  return true;
}

void PoolHttpServer::stop()
{
  postQuitOperation(Base_);
  for (size_t i = 0; i < ThreadsNum_; i++) {
    CLOG_F(INFO, "http worker {} finishing", i);
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
    CLOG_F(ERROR, "HTTP api accept connection failed");
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
