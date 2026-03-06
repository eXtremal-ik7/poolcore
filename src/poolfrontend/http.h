#pragma once

#include "poolconfig/config.h"
#include "poolcore/backend.h"
#include "poolcore/complexMiningStats.h"
#include "poolapi.idl.h"
#include "httpFunctions.idl.h"
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

  using FunctionTy = EHttpFunction;

  struct FunctionMeta {
    bool HasSession;
    bool IsWrite;
    bool IsSuperUser;
  };

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
  void onBackendQueryCoins(const CBackendQueryCoinsRequest&);
  void onBackendQueryPoolStats(const CBackendQueryPoolStatsRequest &request);
  void onBackendQueryPoolStatsHistory(const CBackendQueryPoolStatsHistoryRequest &request, StatisticDb &statistic);
  void onBackendQueryPoolBalance();
  void onInstanceEnumerateAll();

  // WithSession
  void onUserLogout(const CSessionRequest &request, const CToken&);
  void onUserQueryMonitoringSession(const CSessionTargetRequest&, const CToken &token);
  void onUserGetCredentials(const CSessionTargetRequest&, const CToken &token);
  void onUserGetSettings(const CSessionTargetRequest&, const CToken &token);
  void onUserUpdateCredentials(const CUserUpdateCredentialsRequest &request, const CToken &token);
  void onUserUpdateSettings(const CUserUpdateSettingsRequest &request, const CToken &token, PoolBackend&);
  void onUserEnumerateAll(const CUserEnumerateAllRequest &request, const CToken &token, StatisticDb &statistic);
  void onUserEnumerateFeePlan(const CSessionRequest&, const CToken &token);
  void onUserQueryFeePlan(const CUserQueryFeePlanRequest &request, const CToken &token);
  void onUserCreateForce(const CUserCreateForceRequest &request, const CToken &token);
  void onUserCreateFeePlan(const CSessionFeePlanRequest &request, const CToken&);
  void onUserUpdateFeePlan(const CUserUpdateFeePlanRequest &request, const CToken&);
  void onUserDeleteFeePlan(const CSessionFeePlanRequest &request, const CToken&);
  void onUserChangeFeePlan(const CUserChangeFeePlanRequest &request, const CToken &token);
  void onUserRenewFeePlanReferralId(const CSessionFeePlanRequest &request, const CToken&);
  void onUserActivate2faInitiate(const CSessionTargetRequest&, const CToken &token);
  void onUserDeactivate2faInitiate(const CSessionTargetRequest&, const CToken &token);
  void onBackendQueryUserBalance(const CBackendQueryUserBalanceRequest &request, const CToken &token);
  void onBackendQueryUserStats(const CBackendQueryUserStatsRequest &request, const CToken &token, StatisticDb &statistic);
  void onBackendQueryUserStatsHistory(const CBackendQueryUserStatsHistoryRequest &request, const CToken &token, StatisticDb &statistic);
  void onBackendQueryWorkerStatsHistory(const CBackendQueryWorkerStatsHistoryRequest &request, const CToken &token, StatisticDb &statistic);
  void onBackendQueryProfitSwitchCoeff(const CSessionRequest&, const CToken&);
  void onComplexMiningStatsGetInfo(const CSessionRequest&, const CToken&);

  // WithBackend
  void onBackendPoolLuck(const CBackendPoolLuckRequest &request, PoolBackend &backend);
  void onBackendQueryFoundBlocks(const CBackendQueryFoundBlocksRequest &request, PoolBackend &backend);

  // WithSessionAndBackend
  void onBackendManualPayout(const CBackendManualPayoutRequest&, const CToken &token, PoolBackend &backend);
  void onBackendQueryPayouts(const CBackendQueryPayoutsRequest &request, const CToken &token, PoolBackend &backend);
  void onBackendQueryPPLNSPayouts(const CBackendQueryPPLNSPayoutsRequest &request, const CToken &token, PoolBackend &backend);
  void onBackendQueryPPLNSAcc(const CBackendQueryPPLNSAccRequest &request, const CToken &token, PoolBackend &backend);
  void onBackendQueryPPSPayouts(const CBackendQueryPPSPayoutsRequest &request, const CToken &token, PoolBackend &backend);
  void onBackendQueryPPSPayoutsAcc(const CBackendQueryPPSPayoutsAccRequest &request, const CToken &token, PoolBackend &backend);
  void onBackendGetConfig(const CSessionCoinRequest&, const CToken&, PoolBackend &backend);
  void onBackendGetPPSState(const CSessionCoinRequest&, const CToken&, PoolBackend &backend);
  void onBackendQueryPPSHistory(const CBackendQueryPPSHistoryRequest &request, const CToken&, PoolBackend &backend);
  void onBackendUpdateConfig(const CBackendUpdateConfigRequest&, const CToken&, PoolBackend &backend);
  void onBackendUpdateProfitSwitchCoeff(const CBackendUpdateProfitSwitchCoeffRequest &request, const CToken&, PoolBackend &backend);
  void onUserAdjustInstantPayoutThreshold(const CUserAdjustInstantPayoutThresholdRequest &request, const CToken&, PoolBackend &backend);

  void queryStatsHistory(StatisticDb *statistic, const std::string &login, const std::string &worker, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, int64_t currentTime);
  void replyWithStatus(const char *status);

  template<typename T>
  void sendReply(const T &response) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);
    response.serialize(stream);
    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
  }

  template<typename T, typename... Ctx>
  void sendReply(const T &response, Ctx... ctx) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);
    response.serialize(stream, ctx...);
    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
  }

  template<typename> struct HandlerTraits;
  template<typename T> struct HandlerTraits<void (PoolHttpConnection::*)(const T&)> {
    using Request = T;
    static constexpr bool NeedSession = false, NeedBackend = false, NeedStatistic = false;
  };
  template<typename T> struct HandlerTraits<void (PoolHttpConnection::*)(const T&, const CToken&)> {
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
  template<typename T> struct HandlerTraits<void (PoolHttpConnection::*)(const T&, const CToken&, PoolBackend&)> {
    using Request = T;
    static constexpr bool NeedSession = true, NeedBackend = true, NeedStatistic = false;
  };
  template<typename T> struct HandlerTraits<void (PoolHttpConnection::*)(const T&, const CToken&, StatisticDb&)> {
    using Request = T;
    static constexpr bool NeedSession = true, NeedBackend = false, NeedStatistic = true;
  };

  template<FunctionMeta Meta, auto Fn>
  void dispatch() {
    using Traits = HandlerTraits<decltype(Fn)>;
    using T = typename Traits::Request;
    constexpr bool HasMoneyContext = requires { typename T::Capture; typename T::MoneyContext; };

    static_assert(Meta.HasSession == Traits::NeedSession,
                  "handler session parameter must match function metadata");
    static_assert(!HasMoneyContext || Traits::NeedBackend || Traits::NeedStatistic,
                  "money context requires backend or statistic for coin info lookup");

    // Phase 1: Parse
    T req;
    typename T::Capture capture;
    if constexpr (HasMoneyContext) {
      if (!req.parse(Context.Request.c_str(), Context.Request.size(), capture)) {
        replyWithStatus("json_format_error");
        return;
      }
    } else if constexpr (!Meta.HasSession && !Traits::NeedBackend && !Traits::NeedStatistic) {
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

    // Phase 2: Access control and backend/statistic lookup
    [[maybe_unused]] CToken tokenInfo;
    if constexpr (Meta.HasSession) {
      std::string targetLogin;
      if constexpr (requires { req.TargetLogin; })
        targetLogin = req.TargetLogin;
      if (!userManager().validateSession(req.Id, targetLogin, tokenInfo, Meta.IsWrite)) {
        replyWithStatus("unknown_id");
        return;
      }
      if constexpr (Meta.IsSuperUser) {
        if constexpr (Meta.IsWrite) {
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

    // Phase 3: Resolve money context
    if constexpr (HasMoneyContext) {
      uint32_t frac;
      if constexpr (Traits::NeedBackend) frac = b->getCoinInfo().FractionalPartSize;
      else frac = st->getCoinInfo().FractionalPartSize;
      if (!T::resolve(req, capture, frac)) {
        replyWithStatus("request_format_error");
        return;
      }
    }

    // Call handler
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

  template<FunctionMeta Meta, auto Fn>
  void dispatchEmpty() {
    static_assert(!Meta.HasSession, "dispatchEmpty is only for plain public functions");
    if (!Context.Request.empty() && Context.Request != "{}") {
      replyWithStatus("json_format_error");
      return;
    }
    (this->*Fn)();
  }

private:
  enum ParseState {
    psUnknown = 0,
    psApi,
    psResolved,
  };


  PoolHttpServer &Server_;
  aioObject *Socket_;

  char buffer[65536];
  HttpRequestParserState ParserState;
  size_t oldDataSize = 0;
  std::atomic<unsigned> Deleted_ = 0;

  struct {
    int method = hmUnknown;
    ParseState parseState = psUnknown;
    std::optional<FunctionTy> Function;
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
