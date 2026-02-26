#pragma once

#include "poolcore/poolCore.h"
#include "poolcore/poolInstance.h"
#include "rapidjson/document.h"
#include <functional>
#include <unordered_map>

class ComplexMiningStats;
class CPriceFetcher;
class StatisticServer;
class UserManager;

class PoolInstanceFabric {
public:
  static CPoolInstance *get(asyncBase *base,
                            UserManager &userMgr,
                            const std::vector<PoolBackend*> &linkedBackends,
                            CThreadPool &pool,
                            StatisticServer *algoMetaStatistic,
                            ComplexMiningStats *miningStats,
                            const std::string &type,
                            const std::string &protocol,
                            unsigned instanceId,
                            unsigned instancesNum,
                            rapidjson::Value &config,
                            CPriceFetcher *priceFetcher,
                            const std::filesystem::path &logsPath);

private:
  using NewPoolInstanceFunction = std::function<CPoolInstance*(
    asyncBase*,
    UserManager&,
    const std::vector<PoolBackend*>&,
    CThreadPool&,
    StatisticServer*,
    ComplexMiningStats*,
    unsigned,
    unsigned,
    rapidjson::Value&,
    CPriceFetcher*,
    const std::filesystem::path&)>;

private:
  static std::unordered_map<std::string, NewPoolInstanceFunction> FabricData_;
};
