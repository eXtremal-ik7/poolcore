#pragma once

#include "poolconfig/config.h"
#include "poolcore/backend.h"
#include "poolcore/complexMiningStats.h"
#include "poolapi.idl.h"
#include <p2putils/HttpRequestParse.h>

class CPriceFetcher;
class PoolHttpServer;

class PoolHttpConnection {
public:
  PoolHttpConnection(PoolHttpServer &server, aioObject *socket) : Server_(server), Socket_(socket) {
    httpRequestParserInit(&ParserState);
    objectSetDestructorCb(aioObjectHandle(Socket_), [](aioObjectRoot*, void *arg) {
      delete static_cast<PoolHttpConnection*>(arg);
    }, this);
  }
  void run();

private:
  static void readCb(AsyncOpStatus status, aioObject*, size_t size, void *arg) { static_cast<PoolHttpConnection*>(arg)->onRead(status, size); }
  static void writeCb(AsyncOpStatus, aioObject*, size_t, void *arg) { static_cast<PoolHttpConnection*>(arg)->onWrite(); }

  void onWrite();
  void onRead(AsyncOpStatus status, size_t);
  int onParse(HttpRequestComponent *component);
  void close();

  void reply200(xmstream &stream);
  void reply404();
  size_t startChunk(xmstream &stream);
  void finishChunk(xmstream &stream, size_t offset);

  UserManager &userManager();
  PoolBackend *backend(const std::string &coin);
  StatisticDb *statistic(const std::string &coin);

  // Plain (no session/backend validation)
  void onUserAction(const CUserActionRequest &request);
  void onUserCreate(const CUserCreateRequest &request);
  void onUserResendEmail(const CUserResendEmailRequest &request);
  void onUserLogin(const CUserLoginRequest &request);
  void onUserChangeEmail();
  void onUserChangePasswordInitiate(const CUserChangePasswordInitiateRequest &request);
  void onUserChangePasswordForce(const CUserChangePasswordForceRequest &request);
  void onBackendQueryCoins(const CBackendQueryCoinsRequest &request);
  void onBackendQueryPoolStats(const CBackendQueryPoolStatsRequest &request);
  void onBackendQueryPoolStatsHistory(const CBackendQueryPoolStatsHistoryRequest &request, StatisticDb &statistic);
  void onBackendQueryPoolBalance();
  void onInstanceEnumerateAll();

  // WithSession
  void onUserLogout(const CUserLogoutRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserQueryMonitoringSession(const CUserQueryMonitoringSessionRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserGetCredentials(const CUserGetCredentialsRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserGetSettings(const CUserGetSettingsRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserUpdateCredentials(const CUserUpdateCredentialsRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserUpdateSettings(const CUserUpdateSettingsRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserEnumerateAll(const CUserEnumerateAllRequest &request, const UserManager::UserWithAccessRights &tokenInfo, StatisticDb &statistic);
  void onUserEnumerateFeePlan(const CUserEnumerateFeePlanRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserQueryFeePlan(const CUserQueryFeePlanRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserCreateForce(const CUserCreateForceRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserCreateFeePlan(const CUserCreateFeePlanRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserUpdateFeePlan(const CUserUpdateFeePlanRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserDeleteFeePlan(const CUserDeleteFeePlanRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserChangeFeePlan(const CUserChangeFeePlanRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserRenewFeePlanReferralId(const CUserRenewFeePlanReferralIdRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserActivate2faInitiate(const CUserActivate2faInitiateRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onUserDeactivate2faInitiate(const CUserDeactivate2faInitiateRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onBackendQueryUserBalance(const CBackendQueryUserBalanceRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onBackendQueryUserStats(const CBackendQueryUserStatsRequest &request, const UserManager::UserWithAccessRights &tokenInfo, StatisticDb &statistic);
  void onBackendQueryUserStatsHistory(const CBackendQueryUserStatsHistoryRequest &request, const UserManager::UserWithAccessRights &tokenInfo, StatisticDb &statistic);
  void onBackendQueryWorkerStatsHistory(const CBackendQueryWorkerStatsHistoryRequest &request, const UserManager::UserWithAccessRights &tokenInfo, StatisticDb &statistic);
  void onBackendQueryProfitSwitchCoeff(const CBackendQueryProfitSwitchCoeffRequest &request, const UserManager::UserWithAccessRights &tokenInfo);
  void onComplexMiningStatsGetInfo(const CComplexMiningStatsGetInfoRequest &request, const UserManager::UserWithAccessRights &tokenInfo);

  // WithBackend
  void onBackendPoolLuck(const CBackendPoolLuckRequest &request, PoolBackend &backend);
  void onBackendQueryFoundBlocks(const CBackendQueryFoundBlocksRequest &request, PoolBackend &backend);

  // WithSessionAndBackend
  void onBackendManualPayout(const CBackendManualPayoutRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend);
  void onBackendQueryPayouts(const CBackendQueryPayoutsRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend);
  void onBackendQueryPPLNSPayouts(const CBackendQueryPPLNSPayoutsRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend);
  void onBackendQueryPPLNSAcc(const CBackendQueryPPLNSAccRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend);
  void onBackendQueryPPSPayouts(const CBackendQueryPPSPayoutsRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend);
  void onBackendQueryPPSPayoutsAcc(const CBackendQueryPPSPayoutsAccRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend);
  void onBackendGetConfig(const CBackendGetConfigRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend);
  void onBackendGetPPSState(const CBackendGetPPSStateRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend);
  void onBackendQueryPPSHistory(const CBackendQueryPPSHistoryRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend);
  void onBackendUpdateConfig(const CBackendUpdateConfigRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend);
  void onBackendUpdateProfitSwitchCoeff(const CBackendUpdateProfitSwitchCoeffRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend);
  void onUserAdjustInstantPayoutThreshold(const CUserAdjustInstantPayoutThresholdRequest &request, const UserManager::UserWithAccessRights &tokenInfo, PoolBackend &backend);

  void queryStatsHistory(StatisticDb *statistic, const std::string &login, const std::string &worker, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, int64_t currentTime);
  void replyWithStatus(const char *status);

  template<typename> struct HandlerTraits;
  template<typename T> struct HandlerTraits<void (PoolHttpConnection::*)(const T&)> {
    using Request = T;
    static constexpr bool NeedSession = false, NeedBackend = false, NeedStatistic = false;
  };
  template<typename T> struct HandlerTraits<void (PoolHttpConnection::*)(const T&, const UserManager::UserWithAccessRights&)> {
    using Request = T;
    static constexpr bool NeedSession = true, NeedBackend = false, NeedStatistic = false;
  };
  template<typename T> struct HandlerTraits<void (PoolHttpConnection::*)(const T&, PoolBackend&)> {
    using Request = T;
    static constexpr bool NeedSession = false, NeedBackend = true, NeedStatistic = false;
  };
  template<typename T> struct HandlerTraits<void (PoolHttpConnection::*)(const T&, StatisticDb&)> {
    using Request = T;
    static constexpr bool NeedSession = false, NeedBackend = false, NeedStatistic = true;
  };
  template<typename T> struct HandlerTraits<void (PoolHttpConnection::*)(const T&, const UserManager::UserWithAccessRights&, PoolBackend&)> {
    using Request = T;
    static constexpr bool NeedSession = true, NeedBackend = true, NeedStatistic = false;
  };
  template<typename T> struct HandlerTraits<void (PoolHttpConnection::*)(const T&, const UserManager::UserWithAccessRights&, StatisticDb&)> {
    using Request = T;
    static constexpr bool NeedSession = true, NeedBackend = false, NeedStatistic = true;
  };

  template<auto Fn>
  void dispatch() {
    using Traits = HandlerTraits<decltype(Fn)>;
    using T = typename Traits::Request;

    T req;
    if constexpr (!Traits::NeedSession && !Traits::NeedBackend && !Traits::NeedStatistic) {
      const char *data = !Context.Request.empty() ? Context.Request.c_str() : "{}";
      size_t size = !Context.Request.empty() ? Context.Request.size() : 2;
      if (!req.parse(data, size)) {
        replyWithStatus("json_format_error");
        return;
      }
    } else {
      if (!req.parse(Context.Request.c_str(), Context.Request.size())) {
        replyWithStatus("json_format_error");
        return;
      }
    }

    [[maybe_unused]] UserManager::UserWithAccessRights tokenInfo;
    if constexpr (Traits::NeedSession) {
      std::string targetLogin;
      if constexpr (requires { req.TargetLogin; })
        targetLogin = req.TargetLogin;
      if (!userManager().validateSession(req.Id, targetLogin, tokenInfo, Context.Function->IsWrite)) {
        replyWithStatus("unknown_id");
        return;
      }
      if (Context.Function->IsSuperUser) {
        if (Context.Function->IsWrite) {
          if (tokenInfo.Login != "admin") {
            replyWithStatus("unknown_id");
            return;
          }
        } else {
          if (tokenInfo.Login != "admin" && tokenInfo.Login != "observer") {
            replyWithStatus("unknown_id");
            return;
          }
        }
      }
    }

    [[maybe_unused]] PoolBackend *b = nullptr;
    if constexpr (Traits::NeedBackend) {
      b = backend(req.Coin);
      if (!b) {
        replyWithStatus("invalid_coin");
        return;
      }
    }

    [[maybe_unused]] StatisticDb *st = nullptr;
    if constexpr (Traits::NeedStatistic) {
      st = statistic(req.Coin);
      if (!st) {
        replyWithStatus("invalid_coin");
        return;
      }
    }

    if constexpr (Traits::NeedSession && Traits::NeedBackend)
      (this->*Fn)(req, tokenInfo, *b);
    else if constexpr (Traits::NeedSession && Traits::NeedStatistic)
      (this->*Fn)(req, tokenInfo, *st);
    else if constexpr (Traits::NeedSession)
      (this->*Fn)(req, tokenInfo);
    else if constexpr (Traits::NeedBackend)
      (this->*Fn)(req, *b);
    else if constexpr (Traits::NeedStatistic)
      (this->*Fn)(req, *st);
    else
      (this->*Fn)(req);
  }

private:
  enum ParseState {
    psUnknown = 0,
    psApi,
    psResolved,
  };

  enum FunctionTy {
    // Backend functions
    fnBackendGetConfig,
    fnBackendGetPPSState,
    fnBackendManualPayout,
    fnBackendPoolLuck,
    fnBackendQueryCoins,
    fnBackendQueryFoundBlocks,
    fnBackendQueryPayouts,
    fnBackendQueryPoolBalance,
    fnBackendQueryPoolStats,
    fnBackendQueryPoolStatsHistory,
    fnBackendQueryPPLNSAcc,
    fnBackendQueryPPLNSPayouts,
    fnBackendQueryPPSHistory,
    fnBackendQueryPPSPayouts,
    fnBackendQueryPPSPayoutsAcc,
    fnBackendQueryProfitSwitchCoeff,
    fnBackendQueryUserBalance,
    fnBackendQueryUserStats,
    fnBackendQueryUserStatsHistory,
    fnBackendQueryWorkerStatsHistory,
    fnBackendUpdateConfig,
    fnBackendUpdateProfitSwitchCoeff,

    // Complex mining stats functions
    fnComplexMiningStatsGetInfo,

    // Instance functions
    fnInstanceEnumerateAll,

    // User manager functions
    fnUserAction,
    fnUserActivate2faInitiate,
    fnUserAdjustInstantPayoutThreshold,
    fnUserChangeEmail,
    fnUserChangeFeePlan,
    fnUserChangePasswordForce,
    fnUserChangePasswordInitiate,
    fnUserCreate,
    fnUserCreateFeePlan,
    fnUserCreateForce,
    fnUserDeactivate2faInitiate,
    fnUserDeleteFeePlan,
    fnUserEnumerateAll,
    fnUserEnumerateFeePlan,
    fnUserGetCredentials,
    fnUserGetSettings,
    fnUserLogin,
    fnUserLogout,
    fnUserQueryFeePlan,
    fnUserQueryMonitoringSession,
    fnUserRenewFeePlanReferralId,
    fnUserResendEmail,
    fnUserUpdateCredentials,
    fnUserUpdateFeePlan,
    fnUserUpdateSettings,
  };

  struct FunctionInfo {
    FunctionTy Type;
    bool HasSession;
    bool IsWrite;
    bool IsSuperUser;
  };

  static std::unordered_map<std::string, FunctionInfo> FunctionNameMap_;

  PoolHttpServer &Server_;
  aioObject *Socket_;

  char buffer[65536];
  HttpRequestParserState ParserState;
  size_t oldDataSize = 0;
  std::atomic<unsigned> Deleted_ = 0;

  struct {
    int method = hmUnknown;
    ParseState parseState = psUnknown;
    const FunctionInfo *Function = nullptr;
    std::string Request;
  } Context;
};

class PoolHttpServer {
public:
  PoolHttpServer(uint16_t port,
                 UserManager &userMgr,
                 std::vector<std::unique_ptr<PoolBackend>> &backends,
                 std::vector<std::unique_ptr<StatisticServer>> &algoMetaStatistic,
                 ComplexMiningStats &complexMiningStats,
                 const CPoolFrontendConfig &config,
                 size_t threadsNum,
                 CPriceFetcher *priceFetcher);

  bool start();
  void stop();

  UserManager &userManager() { return UserMgr_; }
  const CPoolFrontendConfig &config() { return Config_; }
  PoolBackend *backend(size_t i) { return Backends_[i]; }
  PoolBackend *backend(const std::string &coin) {
    auto It = std::lower_bound(Backends_.begin(), Backends_.end(), coin, [](const auto &backend, const std::string &name) { return backend->getCoinInfo().Name < name; });
    if (It == Backends_.end() || (*It)->getCoinInfo().Name != coin)
      return nullptr;

    return *It;
  }

  StatisticDb *statisticDb(const std::string &name) {
    auto It = std::lower_bound(Statistic_.begin(), Statistic_.end(), name, [](const auto &statistic, const std::string &name) { return statistic->getCoinInfo().Name < name; });
    if (It == Statistic_.end() || (*It)->getCoinInfo().Name != name)
      return nullptr;

    return *It;
  }

  std::vector<PoolBackend*> &backends() { return Backends_; }
  std::vector<StatisticDb*> &statistics() { return Statistic_; }
  ComplexMiningStats &miningStats() { return MiningStats_; }
  CPriceFetcher *priceFetcher() { return PriceFetcher_; }

private:
  static void acceptCb(AsyncOpStatus status, aioObject *object, HostAddress, socketTy socketFd, void *arg);

  void onAccept(AsyncOpStatus status, aioObject *object);

private:
  asyncBase *Base_;
  uint16_t Port_;
  UserManager &UserMgr_;
  ComplexMiningStats &MiningStats_;
  const CPoolFrontendConfig &Config_;
  size_t ThreadsNum_;
  CPriceFetcher *PriceFetcher_;
  std::vector<PoolBackend*> Backends_;
  std::vector<StatisticDb*> Statistic_;

  std::unique_ptr<std::thread[]> Threads_;
  aioObject *ListenerSocket_;
};
