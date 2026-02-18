#pragma once

#include "config.h"
#include "poolcore/backend.h"
#include "poolcore/complexMiningStats.h"
#include <p2putils/HttpRequestParse.h>

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

  void onUserAction(rapidjson::Document &document);
  void onUserCreate(rapidjson::Document &document);
  void onUserResendEmail(rapidjson::Document &document);
  void onUserLogin(rapidjson::Document &document);
  void onUserLogout(rapidjson::Document &document);
  void onUserQueryMonitoringSession(rapidjson::Document &document);
  void onUserChangeEmail(rapidjson::Document &document);
  void onUserChangePasswordInitiate(rapidjson::Document &document);
  void onUserChangePasswordForce(rapidjson::Document &document);
  void onUserGetCredentials(rapidjson::Document &document);
  void onUserGetSettings(rapidjson::Document &document);
  void onUserUpdateCredentials(rapidjson::Document &document);
  void onUserUpdateSettings(rapidjson::Document &document);
  void onUserEnumerateAll(rapidjson::Document &document);
  void onUserEnumerateFeePlan(rapidjson::Document &document);
  void onUserCreateFeePlan(rapidjson::Document &document);
  void onUserQueryFeePlan(rapidjson::Document &document);
  void onUserUpdateFeePlan(rapidjson::Document &document);
  void onUserDeleteFeePlan(rapidjson::Document &document);
  void onUserChangeFeePlan(rapidjson::Document &document);
  void onUserRenewFeePlanReferralId(rapidjson::Document &document);
  void onUserActivate2faInitiate(rapidjson::Document &document);
  void onUserDeactivate2faInitiate(rapidjson::Document &document);

  void onBackendManualPayout(rapidjson::Document &document);
  void onBackendQueryUserBalance(rapidjson::Document &document);
  void onBackendQueryUserStats(rapidjson::Document &document);
  void onBackendQueryUserStatsHistory(rapidjson::Document &document);
  void onBackendQueryWorkerStatsHistory(rapidjson::Document &document);
  void onBackendQueryCoins(rapidjson::Document &document);
  void onBackendQueryFoundBlocks(rapidjson::Document &document);
  void onBackendQueryPayouts(rapidjson::Document &document);
  void onBackendQueryPoolBalance(rapidjson::Document &document);
  void onBackendQueryPoolStats(rapidjson::Document &document);
  void onBackendQueryPoolStatsHistory(rapidjson::Document &document);
  void onBackendQueryProfitSwitchCoeff(rapidjson::Document &document);
  void onBackendQueryPPLNSPayouts(rapidjson::Document &document);
  void onBackendQueryPPLNSAcc(rapidjson::Document &document);
  void onBackendQueryPPSPayouts(rapidjson::Document &document);
  void onBackendQueryPPSPayoutsAcc(rapidjson::Document &document);
  void onBackendUpdateProfitSwitchCoeff(rapidjson::Document &document);
  void onBackendGetPPSConfig(rapidjson::Document &document);
  void onBackendUpdatePPSConfig(rapidjson::Document &document);
  void onBackendPoolLuck(rapidjson::Document &document);

  void onInstanceEnumerateAll(rapidjson::Document &document);

  void onComplexMiningStatsGetInfo(rapidjson::Document &document);

  void queryStatsHistory(StatisticDb *statistic, const std::string &login, const std::string &worker, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval, int64_t currentTime);
  void replyWithStatus(const char *status);

private:
  enum FunctionTy {
    fnUnknown = 0,
    fnApi,
    // User manager functions
    fnUserAction,
    fnUserCreate,
    fnUserResendEmail,
    fnUserLogin,
    fnUserLogout,
    fnUserQueryMonitoringSession,
    fnUserChangeEmail,
    fnUserChangePasswordInitiate,
    fnUserChangePasswordForce,
    fnUserGetCredentials,
    fnUserGetSettings,
    fnUserUpdateCredentials,
    fnUserUpdateSettings,
    fnUserEnumerateAll,
    fnUserEnumerateFeePlan,
    fnUserCreateFeePlan,
    fnUserQueryFeePlan,
    fnUserUpdateFeePlan,
    fnUserDeleteFeePlan,
    fnUserChangeFeePlan,
    fnUserRenewFeePlanReferralId,
    fnUserActivate2faInitiate,
    fnUserDeactivate2faInitiate,

    // Backend functions
    fnBackendManualPayout,
    fnBackendQueryCoins,
    fnBackendQueryUserBalance,
    fnBackendQueryUserStats,
    fnBackendQueryUserStatsHistory,
    fnBackendQueryWorkerStatsHistory,
    fnBackendQueryFoundBlocks,
    fnBackendQueryPayouts,
    fnBackendQueryPoolBalance,
    fnBackendQueryPoolStats,
    fnBackendQueryPoolStatsHistory,
    fnBackendQueryProfitSwitchCoeff,
    fnBackendQueryPPLNSPayouts,
    fnBackendQueryPPLNSAcc,
    fnBackendQueryPPSPayouts,
    fnBackendQueryPPSPayoutsAcc,
    fnBackendUpdateProfitSwitchCoeff,
    fnBackendGetPPSConfig,
    fnBackendUpdatePPSConfig,
    fnBackendPoolLuck,

    // Instance functions
    fnInstanceEnumerateAll,

    // Complex mining stats functions
    fnComplexMiningStatsGetInfo
  };

  static std::unordered_map<std::string, std::pair<int, PoolHttpConnection::FunctionTy>> FunctionNameMap_;

  PoolHttpServer &Server_;
  aioObject *Socket_;

  char buffer[65536];
  HttpRequestParserState ParserState;
  size_t oldDataSize = 0;
  std::atomic<unsigned> Deleted_ = 0;

  struct {
    int method = hmUnknown;
    FunctionTy function = fnUnknown;
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
                 size_t threadsNum);

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
  std::vector<PoolBackend*> Backends_;
  std::vector<StatisticDb*> Statistic_;

  std::unique_ptr<std::thread[]> Threads_;
  aioObject *ListenerSocket_;
};
