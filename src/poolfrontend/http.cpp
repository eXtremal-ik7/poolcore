#include "http.h"
#include "poolcommon/utils.h"
#include "poolcore/priceFetcher.h"
#include "poolcore/thread.h"
#include "asyncio/coroutine.h"
#include "asyncio/socket.h"
#include "loguru.hpp"
#include <cmath>

static consteval PoolHttpConnection::FunctionMeta getFunctionMeta(EHttpFunction f)
{
  using enum PoolHttpConnection::EAuthLevel;
  using F = EHttpFunction;
  switch (f) {
    // Public (no session)
    case F::BackendQueryCoins:
    case F::BackendPoolLuck:
    case F::BackendQueryFoundBlocks:
    case F::BackendQueryPoolBalance:
    case F::BackendQueryPoolStats:
    case F::BackendQueryPoolStatsHistory:
    case F::InstanceEnumerateAll:
    case F::UserAction:
    case F::UserChangeEmail:
    case F::UserChangePasswordInitiate:
    case F::UserCreate:
    case F::UserLogin:
    case F::UserResendEmail:
      return {Public, false};

    // Session, Read
    case F::BackendQueryPayouts:
    case F::BackendQueryPPLNSAcc:
    case F::BackendQueryPPLNSPayouts:
    case F::BackendQueryPPSPayouts:
    case F::BackendQueryPPSPayoutsAcc:
    case F::BackendQueryUserBalance:
    case F::BackendQueryUserStats:
    case F::BackendQueryUserStatsHistory:
    case F::BackendQueryWorkerStatsHistory:
    case F::UserEnumerateAll:
    case F::UserEnumerateFeePlan:
    case F::UserGetCredentials:
    case F::UserGetSettings:
    case F::UserLogout:
    case F::UserQueryFeePlan:
    case F::UserQueryMonitoringSession:
      return {Session, false};

    // Session, Write
    case F::BackendManualPayout:
    case F::UserActivate2faInitiate:
    case F::UserDeactivate2faInitiate:
    case F::UserUpdateCredentials:
    case F::UserUpdateSettings:
      return {Session, true};

    // SuperUser, Read (admin + observer)
    case F::BackendGetConfig:
    case F::BackendGetPPSState:
    case F::BackendQueryPPSHistory:
    case F::BackendQueryProfitSwitchCoeff:
    case F::ComplexMiningStatsGetInfo:
      return {SuperUser, false};

    // SuperUser, Write (admin only)
    case F::BackendUpdateConfig:
    case F::BackendUpdateProfitSwitchCoeff:
    case F::UserAdjustInstantPayoutThreshold:
    case F::UserChangePasswordForce:
    case F::UserChangeFeePlan:
    case F::UserCreateFeePlan:
    case F::UserCreateForce:
    case F::UserDeleteFeePlan:
    case F::UserRenewFeePlanReferralId:
    case F::UserUpdateFeePlan:
      return {SuperUser, true};
    default:
      throw "unknown FunctionTy in getFunctionMeta";
  }
}

static constexpr size_t MaxRequestBodySize = 256 * 1024;

static inline bool rawcmp(Raw data, const char *operand) {
  size_t opSize = strlen(operand);
  return data.size == opSize && memcmp(data.data, operand, opSize) == 0;
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
    Context.Function.reset();
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

      EHttpFunction fn;
      if (!parseEHttpFunction(component->data.data, component->data.size, fn)) {
        reply404();
        return 0;
      }

      Context.Function = fn;
      Context.parseState = psResolved;
      return 1;
    } else {
      reply404();
      return 0;
    }
  } else if (component->type == httpRequestDtData) {
    Context.Request.append(component->data.data, component->data.data + component->data.size);
    if (Context.Request.size() > MaxRequestBodySize)
      return 0;
    return 1;
  } else if (component->type == httpRequestDtDataLast) {
    Context.Request.append(component->data.data, component->data.data + component->data.size);
    if (Context.Request.size() > MaxRequestBodySize)
      return 0;

    if (!Context.Function) {
      reply404();
      return 0;
    }

    using F = EHttpFunction;
    switch (*Context.Function) {
      // Plain
      case F::BackendQueryCoins: dispatch<getFunctionMeta(F::BackendQueryCoins), &PoolHttpConnection::onBackendQueryCoins>(); break;
      case F::BackendQueryPoolBalance: dispatchEmpty<getFunctionMeta(F::BackendQueryPoolBalance), &PoolHttpConnection::onBackendQueryPoolBalance>(); break;
      case F::BackendQueryPoolStats: dispatch<getFunctionMeta(F::BackendQueryPoolStats), &PoolHttpConnection::onBackendQueryPoolStats>(); break;
      case F::InstanceEnumerateAll: dispatchEmpty<getFunctionMeta(F::InstanceEnumerateAll), &PoolHttpConnection::onInstanceEnumerateAll>(); break;
      case F::UserAction: dispatch<getFunctionMeta(F::UserAction), &PoolHttpConnection::onUserAction>(); break;
      case F::UserChangeEmail: dispatchEmpty<getFunctionMeta(F::UserChangeEmail), &PoolHttpConnection::onUserChangeEmail>(); break;
      case F::UserChangePasswordInitiate: dispatch<getFunctionMeta(F::UserChangePasswordInitiate), &PoolHttpConnection::onUserChangePasswordInitiate>(); break;
      case F::UserCreate: dispatch<getFunctionMeta(F::UserCreate), &PoolHttpConnection::onUserCreate>(); break;
      case F::UserLogin: dispatch<getFunctionMeta(F::UserLogin), &PoolHttpConnection::onUserLogin>(); break;
      case F::UserResendEmail: dispatch<getFunctionMeta(F::UserResendEmail), &PoolHttpConnection::onUserResendEmail>(); break;
      // Backend
      case F::BackendPoolLuck: dispatch<getFunctionMeta(F::BackendPoolLuck), &PoolHttpConnection::onBackendPoolLuck>(); break;
      case F::BackendQueryFoundBlocks: dispatch<getFunctionMeta(F::BackendQueryFoundBlocks), &PoolHttpConnection::onBackendQueryFoundBlocks>(); break;
      // Statistic
      case F::BackendQueryPoolStatsHistory: dispatch<getFunctionMeta(F::BackendQueryPoolStatsHistory), &PoolHttpConnection::onBackendQueryPoolStatsHistory>(); break;
      // Session
      case F::BackendQueryProfitSwitchCoeff: dispatch<getFunctionMeta(F::BackendQueryProfitSwitchCoeff), &PoolHttpConnection::onBackendQueryProfitSwitchCoeff>(); break;
      case F::BackendQueryUserBalance: dispatch<getFunctionMeta(F::BackendQueryUserBalance), &PoolHttpConnection::onBackendQueryUserBalance>(); break;
      case F::ComplexMiningStatsGetInfo: dispatch<getFunctionMeta(F::ComplexMiningStatsGetInfo), &PoolHttpConnection::onComplexMiningStatsGetInfo>(); break;
      case F::UserActivate2faInitiate: dispatch<getFunctionMeta(F::UserActivate2faInitiate), &PoolHttpConnection::onUserActivate2faInitiate>(); break;
      case F::UserChangeFeePlan: dispatch<getFunctionMeta(F::UserChangeFeePlan), &PoolHttpConnection::onUserChangeFeePlan>(); break;
      case F::UserChangePasswordForce: dispatch<getFunctionMeta(F::UserChangePasswordForce), &PoolHttpConnection::onUserChangePasswordForce>(); break;
      case F::UserCreateFeePlan: dispatch<getFunctionMeta(F::UserCreateFeePlan), &PoolHttpConnection::onUserCreateFeePlan>(); break;
      case F::UserCreateForce: dispatch<getFunctionMeta(F::UserCreateForce), &PoolHttpConnection::onUserCreateForce>(); break;
      case F::UserDeactivate2faInitiate: dispatch<getFunctionMeta(F::UserDeactivate2faInitiate), &PoolHttpConnection::onUserDeactivate2faInitiate>(); break;
      case F::UserDeleteFeePlan: dispatch<getFunctionMeta(F::UserDeleteFeePlan), &PoolHttpConnection::onUserDeleteFeePlan>(); break;
      case F::UserEnumerateFeePlan: dispatch<getFunctionMeta(F::UserEnumerateFeePlan), &PoolHttpConnection::onUserEnumerateFeePlan>(); break;
      case F::UserGetCredentials: dispatch<getFunctionMeta(F::UserGetCredentials), &PoolHttpConnection::onUserGetCredentials>(); break;
      case F::UserGetSettings: dispatch<getFunctionMeta(F::UserGetSettings), &PoolHttpConnection::onUserGetSettings>(); break;
      case F::UserLogout: dispatch<getFunctionMeta(F::UserLogout), &PoolHttpConnection::onUserLogout>(); break;
      case F::UserQueryFeePlan: dispatch<getFunctionMeta(F::UserQueryFeePlan), &PoolHttpConnection::onUserQueryFeePlan>(); break;
      case F::UserQueryMonitoringSession: dispatch<getFunctionMeta(F::UserQueryMonitoringSession), &PoolHttpConnection::onUserQueryMonitoringSession>(); break;
      case F::UserRenewFeePlanReferralId: dispatch<getFunctionMeta(F::UserRenewFeePlanReferralId), &PoolHttpConnection::onUserRenewFeePlanReferralId>(); break;
      case F::UserUpdateCredentials: dispatch<getFunctionMeta(F::UserUpdateCredentials), &PoolHttpConnection::onUserUpdateCredentials>(); break;
      case F::UserUpdateFeePlan: dispatch<getFunctionMeta(F::UserUpdateFeePlan), &PoolHttpConnection::onUserUpdateFeePlan>(); break;
      case F::UserUpdateSettings: dispatch<getFunctionMeta(F::UserUpdateSettings), &PoolHttpConnection::onUserUpdateSettings>(); break;
      // Session + Statistic
      case F::BackendQueryUserStats: dispatch<getFunctionMeta(F::BackendQueryUserStats), &PoolHttpConnection::onBackendQueryUserStats>(); break;
      case F::BackendQueryUserStatsHistory: dispatch<getFunctionMeta(F::BackendQueryUserStatsHistory), &PoolHttpConnection::onBackendQueryUserStatsHistory>(); break;
      case F::BackendQueryWorkerStatsHistory: dispatch<getFunctionMeta(F::BackendQueryWorkerStatsHistory), &PoolHttpConnection::onBackendQueryWorkerStatsHistory>(); break;
      case F::UserEnumerateAll: dispatch<getFunctionMeta(F::UserEnumerateAll), &PoolHttpConnection::onUserEnumerateAll>(); break;
      // Session + Backend
      case F::BackendGetConfig: dispatch<getFunctionMeta(F::BackendGetConfig), &PoolHttpConnection::onBackendGetConfig>(); break;
      case F::BackendGetPPSState: dispatch<getFunctionMeta(F::BackendGetPPSState), &PoolHttpConnection::onBackendGetPPSState>(); break;
      case F::BackendManualPayout: dispatch<getFunctionMeta(F::BackendManualPayout), &PoolHttpConnection::onBackendManualPayout>(); break;
      case F::BackendQueryPayouts: dispatch<getFunctionMeta(F::BackendQueryPayouts), &PoolHttpConnection::onBackendQueryPayouts>(); break;
      case F::BackendQueryPPLNSAcc: dispatch<getFunctionMeta(F::BackendQueryPPLNSAcc), &PoolHttpConnection::onBackendQueryPPLNSAcc>(); break;
      case F::BackendQueryPPLNSPayouts: dispatch<getFunctionMeta(F::BackendQueryPPLNSPayouts), &PoolHttpConnection::onBackendQueryPPLNSPayouts>(); break;
      case F::BackendQueryPPSHistory: dispatch<getFunctionMeta(F::BackendQueryPPSHistory), &PoolHttpConnection::onBackendQueryPPSHistory>(); break;
      case F::BackendQueryPPSPayouts: dispatch<getFunctionMeta(F::BackendQueryPPSPayouts), &PoolHttpConnection::onBackendQueryPPSPayouts>(); break;
      case F::BackendQueryPPSPayoutsAcc: dispatch<getFunctionMeta(F::BackendQueryPPSPayoutsAcc), &PoolHttpConnection::onBackendQueryPPSPayoutsAcc>(); break;
      case F::BackendUpdateConfig: dispatch<getFunctionMeta(F::BackendUpdateConfig), &PoolHttpConnection::onBackendUpdateConfig>(); break;
      case F::BackendUpdateProfitSwitchCoeff: dispatch<getFunctionMeta(F::BackendUpdateProfitSwitchCoeff), &PoolHttpConnection::onBackendUpdateProfitSwitchCoeff>(); break;
      case F::UserAdjustInstantPayoutThreshold: dispatch<getFunctionMeta(F::UserAdjustInstantPayoutThreshold), &PoolHttpConnection::onUserAdjustInstantPayoutThreshold>(); break;
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
  const char reply200[] = "HTTP/1.1 200 OK\r\nServer: bcnode\r\nContent-Type: application/json\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n";
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
  stream.write(finishData, sizeof(finishData) - 1);
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
  credentials.ReferralId = request.ReferralId;

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userCreate("", std::move(credentials), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserCreateForce(const CUserCreateForceRequest &request, const CToken &token)
{
  UserManager::Credentials credentials = credentialsFromRequest(request);

  if (!credentials.FeePlan.empty() && !credentials.ReferralId.empty()) {
    replyWithStatus("request_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userCreate(token.Login, std::move(credentials), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserResendEmail(const CUserResendEmailRequest &request)
{
  UserManager::Credentials credentials;
  credentials.Login = request.Login;
  credentials.Password = request.Password;

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userResendEmail(std::move(credentials), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserLogin(const CUserLoginRequest &request)
{
  UserManager::Credentials credentials;
  credentials.Login = request.Login;
  credentials.Password = request.Password;
  credentials.TwoFactor = request.Totp;

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userLogin(std::move(credentials), [this](const std::string &sessionId, const char *status, bool isReadOnly) {
    sendReply<CUserLoginResponse>(status, sessionId, isReadOnly);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserLogout(const CSessionRequest &request, const CToken&)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userLogout(request.Id, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserQueryMonitoringSession(const CSessionTargetRequest&, const CToken &token)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userQueryMonitoringSession(token.Login, [this](const std::string &sessionId, const char *status) {
    sendReply<CUserQueryMonitoringSessionResponse>(status, sessionId);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserChangeEmail()
{
  replyWithStatus("not_implemented");
}

void PoolHttpConnection::onUserChangePasswordInitiate(const CUserChangePasswordInitiateRequest &request)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userChangePasswordInitiate(request.Login, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserChangePasswordForce(const CUserChangePasswordForceRequest &request, const CToken &token)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userChangePasswordForce(token.Login, request.NewPassword, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserGetCredentials(const CSessionTargetRequest&, const CToken &token)
{
  UserManager::Credentials c;
  if (!Server_.userManager().getUserCredentials(token.Login, c)) {
    replyWithStatus("unknown_id");
    return;
  }

  bool isReadOnly = token.IsReadOnly || c.IsReadOnly;
  int64_t regDate = c.RegistrationDate.toUnixTime();
  sendReply<CUserGetCredentialsResponse>("ok", token.Login, c.Name, c.EMail, regDate, c.IsActive, isReadOnly, c.HasTwoFactor);
}

void PoolHttpConnection::onUserGetSettings(const CSessionTargetRequest&, const CToken &token)
{
  std::string feePlanId = Server_.userManager().getFeePlanId(token.Login);

  std::vector<CCoinSettings> coins;
  std::vector<uint32_t> moneyCtx;

  for (const auto &backend: Server_.backends()) {
    const CCoinInfo &coinInfo = backend->getCoinInfo();
    UserSettingsRecord settings;
    Server_.userManager().getUserCoinSettings(token.Login, coinInfo.Name, settings);

    double pplnsFee = 0.0;
    for (const auto &fee : Server_.userManager().getFeeRecord(feePlanId, EMiningMode::Pplns, coinInfo.Name))
      pplnsFee += fee.Percentage;
    double ppsFee = backend->accountingDb()->backendSettings().PPSConfig.PoolFee;
    for (const auto &fee : Server_.userManager().getFeeRecord(feePlanId, EMiningMode::Pps, coinInfo.Name))
      ppsFee += fee.Percentage;

    auto &coin = coins.emplace_back();
    coin.Name = coinInfo.Name;
    coin.Payout = settings.Payout;
    coin.Mining = settings.Mining;
    coin.AutoExchange = settings.AutoExchange;
    coin.PplnsFee = pplnsFee;
    coin.PpsFee = ppsFee;
    moneyCtx.push_back(coinInfo.FractionalPartSize);
  }

  sendReply<CUserGetSettingsResponse>("ok", coins, moneyCtx);
}

void PoolHttpConnection::onUserUpdateCredentials(const CUserUpdateCredentialsRequest &request, const CToken &token)
{
  UserManager::Credentials credentials;
  credentials.Name = request.Name;

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().updateCredentials(token.Login, std::move(credentials), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserUpdateSettings(const CUserUpdateSettingsRequest &request, const CToken &token, PoolBackend&)
{
  if (!request.Payout.has_value() && !request.Mining.has_value() && !request.AutoExchange.has_value()) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().updateSettings(token.Login,
                                       request.Coin,
                                       request.Payout,
                                       request.Mining,
                                       request.AutoExchange,
                                       request.Totp,
                                       [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserEnumerateAll(const CUserEnumerateAllRequest &request, const CToken &token, StatisticDb &statistic)
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
  Server_.userManager().enumerateUsers(token.Login, [this, &statistic, offset, size, column, sortDescending](const char *status, std::vector<UserManager::Credentials> &allUsers) {
    statistic.queryAllusersStats(std::move(allUsers), [this, status](const std::vector<StatisticDb::CredentialsWithStatistic> &result) {
      std::vector<CUserWithStats> users;
      for (const auto &user: result) {
        CUserWithStats entry;
        entry.Login = user.Credentials.Login;
        entry.Name = user.Credentials.Name;
        entry.Email = user.Credentials.EMail;
        entry.RegistrationDate = user.Credentials.RegistrationDate.toUnixTime();
        entry.IsActive = user.Credentials.IsActive;
        entry.IsReadOnly = user.Credentials.IsReadOnly;
        entry.FeePlanId = user.Credentials.FeePlan;
        entry.Workers = user.WorkersNum;
        entry.ShareRate = user.SharesPerSecond;
        entry.Power = user.AveragePower;
        entry.LastShareTime = user.LastShareTime.toUnixTime();
        users.push_back(std::move(entry));
      }
      sendReply<CUserEnumerateAllResponse>(status, users);
      objectDecrementReference(aioObjectHandle(Socket_), 1);

    }, offset, size, column, sortDescending);
  });
}


void PoolHttpConnection::onUserCreateFeePlan(const CSessionFeePlanRequest &request, const CToken&)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().createFeePlan(request.FeePlanId, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserUpdateFeePlan(const CUserUpdateFeePlanRequest &request, const CToken&)
{
  EMiningMode mode = request.Mode;

  CModeFeeConfig modeConfig;
  modeConfig.Default = request.Default;
  modeConfig.CoinSpecific = request.CoinSpecific;

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

void PoolHttpConnection::onUserDeleteFeePlan(const CSessionFeePlanRequest &request, const CToken&)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().deleteFeePlan(request.FeePlanId, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserEnumerateFeePlan(const CSessionRequest&, const CToken &token)
{
  std::string status;
  std::vector<std::string> result;
  if (!Server_.userManager().enumerateFeePlan(token.Login, status, result)) {
    replyWithStatus(status.c_str());
    return;
  }

  sendReply<CUserEnumerateFeePlanResponse>("ok", result);
}

void PoolHttpConnection::onUserQueryFeePlan(const CUserQueryFeePlanRequest &request, const CToken &token)
{
  EMiningMode mode = request.Mode;

  std::string status;
  CModeFeeConfig result;
  BaseBlob<256> referralId;
  if (!Server_.userManager().queryFeePlan(token.Login, request.FeePlanId, mode, status, referralId, result)) {
    replyWithStatus(status.c_str());
    return;
  }

  std::string refIdStr = referralId.isNull() ? "" : referralId.getHexRaw();
  sendReply<CUserQueryFeePlanResponse>("ok", request.FeePlanId, mode, refIdStr, result.Default, result.CoinSpecific);
}

void PoolHttpConnection::onUserChangeFeePlan(const CUserChangeFeePlanRequest &request, const CToken &token)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().changeFeePlan(token.Login, request.FeePlanId, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserRenewFeePlanReferralId(const CSessionFeePlanRequest &request, const CToken&)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().renewFeePlanReferralId(request.FeePlanId, [this](const char *status, const std::string &referralId) {
    sendReply<CUserRenewFeePlanReferralIdResponse>(status, referralId);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserActivate2faInitiate(const CSessionTargetRequest&, const CToken &token)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().activate2faInitiate(token.Login, [this](const char *status, const char *key) {
    sendReply<CUserActivate2faInitiateResponse>(status, key);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserDeactivate2faInitiate(const CSessionTargetRequest&, const CToken &token)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().deactivate2faInitiate(token.Login, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserAdjustInstantPayoutThreshold(const CUserAdjustInstantPayoutThresholdRequest &request, const CToken&, PoolBackend &backend)
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

void PoolHttpConnection::onBackendManualPayout(const CBackendManualPayoutRequest&, const CToken &token, PoolBackend &backend)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend.accountingDb()->manualPayout(token.Login, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryUserBalance(const CBackendQueryUserBalanceRequest &request, const CToken &token)
{
  if (!request.Coin.empty()) {
    PoolBackend *backend = Server_.backend(request.Coin);
    if (!backend) {
      replyWithStatus("invalid_coin");
      return;
    }

    objectIncrementReference(aioObjectHandle(Socket_), 1);
    backend->accountingDb()->queryUserBalance(token.Login, [this, backend](const UserBalanceInfo &record) {
      const CCoinInfo &coinInfo = backend->getCoinInfo();
      std::vector<CBalanceEntry> balances(1);
      balances[0].Coin = coinInfo.Name;
      balances[0].Balance = FormatMoney(record.Data.Balance, coinInfo.FractionalPartSize);
      balances[0].Requested = FormatMoney(record.Data.Requested, coinInfo.FractionalPartSize);
      balances[0].Paid = FormatMoney(record.Data.Paid, coinInfo.FractionalPartSize);
      balances[0].Queued = FormatMoney(record.Queued, coinInfo.FractionalPartSize);
      sendReply<CBackendQueryUserBalanceResponse>("ok", balances);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
  } else {
    // Ask all backends about balances
    objectIncrementReference(aioObjectHandle(Socket_), 1);

    std::vector<AccountingDb*> accountingDbs(Server_.backends().size());
    for (size_t i = 0, ie = Server_.backends().size(); i != ie; ++i)
      accountingDbs[i] = Server_.backend(i)->accountingDb();

    AccountingDb::queryUserBalanceMulti(&accountingDbs[0], accountingDbs.size(), token.Login, [this](const UserBalanceInfo *balanceData, size_t backendsNum) {
      std::vector<CBalanceEntry> balances;
      for (size_t i = 0; i < backendsNum; i++) {
        const CCoinInfo &coinInfo = Server_.backend(i)->getCoinInfo();
        CBalanceEntry entry;
        entry.Coin = coinInfo.Name;
        entry.Balance = FormatMoney(balanceData[i].Data.Balance, coinInfo.FractionalPartSize);
        entry.Requested = FormatMoney(balanceData[i].Data.Requested, coinInfo.FractionalPartSize);
        entry.Paid = FormatMoney(balanceData[i].Data.Paid, coinInfo.FractionalPartSize);
        entry.Queued = FormatMoney(balanceData[i].Queued, coinInfo.FractionalPartSize);
        balances.push_back(std::move(entry));
      }
      sendReply<CBackendQueryUserBalanceResponse>("ok", balances);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
  }
}

void PoolHttpConnection::onBackendQueryUserStats(const CBackendQueryUserStatsRequest &request, const CToken &token, StatisticDb &statistic)
{
  StatisticDb::EStatsColumn column;
  switch (request.SortBy) {
    case EWorkerSortColumn::Name: column = StatisticDb::EStatsColumnName; break;
    case EWorkerSortColumn::AveragePower: column = StatisticDb::EStatsColumnAveragePower; break;
    case EWorkerSortColumn::SharesPerSecond: column = StatisticDb::EStatsColumnSharesPerSecond; break;
    case EWorkerSortColumn::LastShareTime: column = StatisticDb::EStatsColumnLastShareTime; break;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  statistic.queryUserStats(token.Login, [this, &statistic](const CStats &aggregate, const std::vector<CStats> &workers) {
    CStatsTotal total;
    total.Clients = aggregate.ClientsNum;
    total.Workers = aggregate.WorkersNum;
    total.ShareRate = aggregate.SharesPerSecond;
    total.ShareWork = aggregate.SharesWork.getDecimal();
    total.Power = aggregate.AveragePower;
    total.LastShareTime = aggregate.LastShareTime.toUnixTime();
    std::vector<CWorkerStats> workersList;
    for (const auto &w : workers) {
      CWorkerStats entry;
      entry.Name = w.WorkerId;
      entry.ShareRate = w.SharesPerSecond;
      entry.ShareWork = w.SharesWork.getDecimal();
      entry.Power = w.AveragePower;
      entry.LastShareTime = w.LastShareTime.toUnixTime();
      workersList.push_back(std::move(entry));
    }
    const CCoinInfo &ci = statistic.getCoinInfo();
    sendReply<CBackendQueryUserStatsResponse>("ok", ci.getPowerUnitName(), ci.PowerMultLog10, time(nullptr), total, workersList);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  }, request.Offset, request.Size, column, request.SortDescending);
}

void PoolHttpConnection::queryStatsHistory(StatisticDb *statistic, const std::string &login, const std::string &worker, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, int64_t currentTime)
{
  std::vector<CStats> stats;
  statistic->getHistory(login, worker, timeFrom, timeTo, groupByInterval, stats);

  const CCoinInfo &ci = statistic->getCoinInfo();
  std::vector<CStatsHistoryEntry> entries;
  for (const auto &s : stats) {
    CStatsHistoryEntry entry;
    entry.Name = s.WorkerId;
    entry.Time = s.Time.toUnixTime();
    entry.ShareRate = s.SharesPerSecond;
    entry.ShareWork = s.SharesWork.getDecimal();
    entry.Power = s.AveragePower;
    entries.push_back(std::move(entry));
  }
  sendReply<CStatsHistoryResponse>("ok", ci.getPowerUnitName(), ci.PowerMultLog10, currentTime, entries);
}

void PoolHttpConnection::onBackendQueryUserStatsHistory(const CBackendQueryUserStatsHistoryRequest &request, const CToken &token, StatisticDb &statistic)
{
  queryStatsHistory(&statistic, token.Login, "", request.TimeFrom, request.TimeTo, request.GroupByInterval, time(nullptr));
}

void PoolHttpConnection::onBackendQueryWorkerStatsHistory(const CBackendQueryWorkerStatsHistoryRequest &request, const CToken &token, StatisticDb &statistic)
{
  queryStatsHistory(&statistic, token.Login, request.WorkerId, request.TimeFrom, request.TimeTo, request.GroupByInterval, time(nullptr));
}

void PoolHttpConnection::onBackendQueryCoins(const CBackendQueryCoinsRequest&)
{
  std::vector<CHttpCoinInfo> coins;
  for (const auto &backend: Server_.backends()) {
    const CCoinInfo &info = backend->getCoinInfo();
    auto backendCfg = backend->accountingDb()->backendSettings();

    CHttpCoinInfo entry;
    entry.Name = info.Name;
    entry.FullName = info.FullName;
    entry.Algorithm = info.Algorithm;
    entry.PpsAvailable = backendCfg.PPSConfig.Enabled;

    // PPLNS fee: sum of default fee plan percentages
    double pplnsFee = 0.0;
    for (const auto &fee : Server_.userManager().getFeeRecord("default", EMiningMode::Pplns, info.Name))
      pplnsFee += fee.Percentage;
    entry.PplnsFee = pplnsFee;

    // PPS fee: pool PPS fee + default fee plan
    double ppsFee = backendCfg.PPSConfig.PoolFee;
    for (const auto &fee : Server_.userManager().getFeeRecord("default", EMiningMode::Pps, info.Name))
      ppsFee += fee.Percentage;
    entry.PpsFee = ppsFee;

    // Minimal payouts
    entry.MinimalRegularPayout = FormatMoney(backendCfg.PayoutConfig.RegularMinimalPayout, info.FractionalPartSize);
    entry.MinimalInstantPayout = FormatMoney(backendCfg.PayoutConfig.InstantMinimalPayout, info.FractionalPartSize);

    // Swap flags
    entry.AcceptIncoming = backendCfg.SwapConfig.AcceptIncoming;
    entry.AcceptOutgoing = backendCfg.SwapConfig.AcceptOutgoing;

    // Current prices (null when no price data)
    CPriceFetcher *priceFetcher = Server_.priceFetcher();
    if (priceFetcher) {
      double rateToBTC = priceFetcher->getPrice(info.Name);
      double btcToUSD = priceFetcher->getBtcUsd();
      if (rateToBTC > 0.0 && btcToUSD > 0.0) {
        entry.ValueBTC = rateToBTC;
        entry.ValueUSD = rateToBTC * btcToUSD;
      }
    }

    coins.push_back(std::move(entry));
  }
  sendReply<CBackendQueryCoinsResponse>("ok", coins);
}

void PoolHttpConnection::onBackendQueryFoundBlocks(const CBackendQueryFoundBlocksRequest &request, PoolBackend &backend)
{
  const CCoinInfo &coinInfo = backend.getCoinInfo();

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend.accountingDb()->queryFoundBlocks(request.HeightFrom, request.HashFrom, request.Count, [this, &coinInfo](const std::vector<FoundBlockRecord> &blocks, const std::vector<CNetworkClient::GetBlockConfirmationsQuery> &confirmations) {
    std::vector<CFoundBlock> result;
    for (size_t i = 0, ie = blocks.size(); i != ie; ++i) {
      CFoundBlock entry;
      entry.Height = blocks[i].Height;
      entry.Hash = !blocks[i].PublicHash.empty() ? blocks[i].PublicHash : blocks[i].Hash;
      entry.Time = blocks[i].Time.toUnixTime();
      entry.Confirmations = confirmations[i].Confirmations;
      entry.GeneratedCoins = FormatMoney(blocks[i].GeneratedCoins, coinInfo.FractionalPartSize);
      entry.FoundBy = blocks[i].FoundBy;
      if (!blocks[i].MergedBlocks.empty())
        entry.MergedBlocks = blocks[i].MergedBlocks;
      result.push_back(std::move(entry));
    }
    sendReply<CBackendQueryFoundBlocksResponse>("ok", result);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryPayouts(const CBackendQueryPayoutsRequest &request, const CToken &token, PoolBackend &backend)
{
  std::vector<PayoutDbRecord> records;
  backend.queryPayouts(token.Login, Timestamp(request.TimeFrom), request.Count, records);
  unsigned fractionalPartSize = backend.getCoinInfo().FractionalPartSize;
  // NOTE: assumes fractionalPartSize <= 8; if a coin has more fractional digits, rateScale < 1 and mulfp precision may suffer
  double rateScale = std::pow(10.0, 8 - static_cast<int>(fractionalPartSize));

  std::vector<CPayoutRecord> payouts;
  for (size_t i = 0, ie = records.size(); i != ie; ++i) {
    CPayoutRecord entry;
    entry.Time = records[i].Time.count();
    entry.Txid = records[i].TransactionId;
    entry.Value = FormatMoney(records[i].Value, fractionalPartSize);
    UInt<384> valueBTC = records[i].Value;
    valueBTC.mulfp(records[i].RateToBTC * rateScale);
    UInt<384> valueUSD = records[i].Value;
    valueUSD.mulfp(records[i].RateToBTC * records[i].RateBTCToUSD * rateScale);
    entry.ValueBTC = FormatMoney(valueBTC, 8);
    entry.ValueUSD = FormatMoney(valueUSD, 8);
    entry.Status = records[i].Status;
    payouts.push_back(std::move(entry));
  }
  sendReply<CBackendQueryPayoutsResponse>("ok", payouts);
}

void PoolHttpConnection::onBackendQueryPoolBalance()
{
  replyWithStatus("not_implemented");
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
      const CCoinInfo &coinInfo = statistic->getCoinInfo();
      std::vector<CPoolStatsEntry> statsVec(1);
      statsVec[0].Coin = coinInfo.Name;
      statsVec[0].PowerUnit = coinInfo.getPowerUnitName();
      statsVec[0].PowerMultLog10 = coinInfo.PowerMultLog10;
      statsVec[0].Clients = record.ClientsNum;
      statsVec[0].Workers = record.WorkersNum;
      statsVec[0].ShareRate = record.SharesPerSecond;
      statsVec[0].ShareWork = record.SharesWork.getDecimal();
      statsVec[0].Power = record.AveragePower;
      statsVec[0].LastShareTime = record.LastShareTime.toUnixTime();
      sendReply<CBackendQueryPoolStatsResponse>("ok", time(nullptr), statsVec);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
  } else {
    // Ask all backends about stats
    objectIncrementReference(aioObjectHandle(Socket_), 1);

    std::vector<StatisticDb*> statisticDbs(Server_.backends().size());
    for (size_t i = 0, ie = Server_.backends().size(); i != ie; ++i)
      statisticDbs[i] = Server_.backend(i)->statisticDb();

    StatisticDb::queryPoolStatsMulti(&statisticDbs[0], statisticDbs.size(), [this](const CStats *stats, size_t backendsNum) {
      std::vector<CPoolStatsEntry> statsVec;
      for (size_t i = 0; i < backendsNum; i++) {
        const CCoinInfo &coinInfo = Server_.backend(i)->getCoinInfo();
        CPoolStatsEntry entry;
        entry.Coin = coinInfo.Name;
        entry.PowerUnit = coinInfo.getPowerUnitName();
        entry.PowerMultLog10 = coinInfo.PowerMultLog10;
        entry.Clients = stats[i].ClientsNum;
        entry.Workers = stats[i].WorkersNum;
        entry.ShareRate = stats[i].SharesPerSecond;
        entry.ShareWork = stats[i].SharesWork.getDecimal();
        entry.Power = stats[i].AveragePower;
        statsVec.push_back(std::move(entry));
      }
      sendReply<CBackendQueryPoolStatsResponse>("ok", time(nullptr), statsVec);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
  }
}

void PoolHttpConnection::onBackendQueryPoolStatsHistory(const CBackendQueryPoolStatsHistoryRequest &request, StatisticDb &statistic)
{
  queryStatsHistory(&statistic, "", "", request.TimeFrom, request.TimeTo, request.GroupByInterval, time(nullptr));
}

void PoolHttpConnection::onBackendQueryProfitSwitchCoeff(const CSessionRequest&, const CToken&)
{
  std::vector<CProfitSwitchCoinEntry> coins;
  for (const auto &backend : Server_.backends()) {
    CProfitSwitchCoinEntry entry;
    entry.Name = backend->getCoinInfo().Name;
    entry.ProfitSwitchCoeff = backend->getProfitSwitchCoeff();
    coins.push_back(std::move(entry));
  }
  sendReply<CBackendQueryProfitSwitchCoeffResponse>("ok", coins);
}

void PoolHttpConnection::onBackendQueryPPLNSPayouts(const CBackendQueryPPLNSPayoutsRequest &request, const CToken &token, PoolBackend &backend)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend.accountingDb()->queryPPLNSPayouts(token.Login, request.TimeFrom, request.HashFrom, request.Count, [this, &backend](const std::vector<CPPLNSPayoutEntry>& result) {
    sendReply<CBackendQueryPPLNSPayoutsResponse>("ok", result, backend.getCoinInfo().FractionalPartSize);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryPPLNSAcc(const CBackendQueryPPLNSAccRequest &request, const CToken &token, PoolBackend &backend)
{
  if (request.TimeTo <= request.TimeFrom ||
      request.GroupByInterval == 0 ||
      (request.TimeTo - request.TimeFrom) % request.GroupByInterval != 0 ||
      (request.TimeTo - request.TimeFrom) / request.GroupByInterval > 3200) {
    replyWithStatus("invalid_interval");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend.accountingDb()->queryPPLNSAcc(token.Login, request.TimeFrom, request.TimeTo, request.GroupByInterval, [this, &backend](const std::vector<CAccumulatedPayoutEntry>& result) {
    sendReply<CBackendQueryPayoutsAccResponse>("ok", result, backend.getCoinInfo().FractionalPartSize);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryPPSPayouts(const CBackendQueryPPSPayoutsRequest &request, const CToken &token, PoolBackend &backend)
{
  auto result = backend.accountingDb()->api().queryPPSPayouts(token.Login, request.TimeFrom, request.Count);

  sendReply<CBackendQueryPPSPayoutsResponse>("ok", result, backend.getCoinInfo().FractionalPartSize);
}

void PoolHttpConnection::onBackendQueryPPSPayoutsAcc(const CBackendQueryPPSPayoutsAccRequest &request, const CToken &token, PoolBackend &backend)
{
  if (request.TimeTo <= request.TimeFrom ||
      request.GroupByInterval == 0 ||
      (request.TimeTo - request.TimeFrom) % request.GroupByInterval != 0 ||
      (request.TimeTo - request.TimeFrom) / request.GroupByInterval > 3200) {
    replyWithStatus("invalid_interval");
    return;
  }

  auto result = backend.accountingDb()->api().queryPPSPayoutsAcc(token.Login, request.TimeFrom, request.TimeTo, request.GroupByInterval);

  sendReply<CBackendQueryPayoutsAccResponse>("ok", result, backend.getCoinInfo().FractionalPartSize);
}

void PoolHttpConnection::onBackendUpdateProfitSwitchCoeff(const CBackendUpdateProfitSwitchCoeffRequest &request, const CToken&, PoolBackend &backend)
{
  backend.setProfitSwitchCoeff(request.ProfitSwitchCoeff);
  replyWithStatus("ok");
}

void PoolHttpConnection::onBackendGetConfig(const CSessionCoinRequest&, const CToken&, PoolBackend &backend)
{
  CBackendSettings settings = backend.accountingDb()->backendSettings();
  unsigned fractionalPartSize = backend.getCoinInfo().FractionalPartSize;

  std::vector<std::string> satFuncs;
  for (const char *name : ESaturationFunctionNames())
    satFuncs.emplace_back(name);
  sendReply<CBackendGetConfigResponse>("ok", settings.PPSConfig, settings.PayoutConfig, settings.SwapConfig, satFuncs, fractionalPartSize);
}

CHttpPPSState::CHttpPPSState(const CPPSState &pps, unsigned fractionalPartSize)
{
  const auto &reward = pps.LastBaseBlockReward;
  Time = pps.Time.toUnixTime();
  Balance.Value = FormatMoney(pps.Balance, fractionalPartSize);
  Balance.InBlocks = CPPSState::balanceInBlocks(pps.Balance, reward);
  RefBalance.Value = FormatMoney(pps.ReferenceBalance, fractionalPartSize);
  RefBalance.InBlocks = CPPSState::balanceInBlocks(pps.ReferenceBalance, reward);
  RefBalance.SqLambda = CPPSState::sqLambda(pps.ReferenceBalance, reward, pps.TotalBlocksFound);
  MinRefBalance.Time = pps.Min.Time.toUnixTime();
  MinRefBalance.Value = FormatMoney(pps.Min.Balance, fractionalPartSize);
  MinRefBalance.InBlocks = CPPSState::balanceInBlocks(pps.Min.Balance, reward);
  MinRefBalance.SqLambda = CPPSState::sqLambda(pps.Min.Balance, reward, pps.Min.TotalBlocksFound);
  MaxRefBalance.Time = pps.Max.Time.toUnixTime();
  MaxRefBalance.Value = FormatMoney(pps.Max.Balance, fractionalPartSize);
  MaxRefBalance.InBlocks = CPPSState::balanceInBlocks(pps.Max.Balance, reward);
  MaxRefBalance.SqLambda = CPPSState::sqLambda(pps.Max.Balance, reward, pps.Max.TotalBlocksFound);
  TotalBlocksFound = pps.TotalBlocksFound;
  OrphanBlocks = pps.OrphanBlocks;
  LastSaturateCoeff = pps.LastSaturateCoeff;
  LastBaseBlockReward = FormatMoney(pps.LastBaseBlockReward, fractionalPartSize);
  LastAverageTxFee = FormatMoney(pps.LastAverageTxFee, fractionalPartSize);
}

void PoolHttpConnection::onBackendGetPPSState(const CSessionCoinRequest&, const CToken&, PoolBackend &backend)
{
  unsigned fractionalPartSize = backend.getCoinInfo().FractionalPartSize;
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend.accountingDb()->queryPPSState([this, fractionalPartSize](const CPPSState &pps) {
    sendReply<CBackendGetPPSStateResponse>("ok", CHttpPPSState(pps, fractionalPartSize));
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryPPSHistory(const CBackendQueryPPSHistoryRequest &request, const CToken&, PoolBackend &backend)
{
  unsigned fractionalPartSize = backend.getCoinInfo().FractionalPartSize;
  auto result = backend.accountingDb()->api().queryPPSHistory(request.TimeFrom, request.TimeTo);

  std::vector<CHttpPPSState> history;
  for (const auto &pps : result)
    history.emplace_back(pps, fractionalPartSize);
  sendReply<CBackendQueryPPSHistoryResponse>("ok", history);
}

void PoolHttpConnection::onBackendUpdateConfig(const CBackendUpdateConfigRequest &request, const CToken&, PoolBackend &backend)
{
  if (!request.Pps.has_value() && !request.Payouts.has_value() && !request.Swap.has_value()) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend.accountingDb()->updateBackendSettings(
    request.Pps, request.Payouts, request.Swap,
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
    sendReply<CBackendPoolLuckResponse>("ok", result);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onInstanceEnumerateAll()
{
  std::vector<CInstanceInfo> instances;
  for (const auto &instance : Server_.config().Instances) {
    CInstanceInfo info;
    info.Protocol = instance.Protocol;
    info.Type = instance.Type;
    info.Port = instance.Port;
    info.Backends = instance.Backends;
    info.ShareDiff = instance.ShareDiff;
    instances.push_back(std::move(info));
  }
  sendReply<CInstanceEnumerateAllResponse>("ok", instances);
}

void PoolHttpConnection::onComplexMiningStatsGetInfo(const CSessionRequest&, const CToken&)
{
  const char *data = Context.Request.c_str();
  size_t size = Context.Request.size();
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.miningStats().query(data, size, [this](const char *data, size_t size) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);
    stream.write(data, size);
    stream.write('\n');
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
  sendReply<CStatusResponse>(status);
}
