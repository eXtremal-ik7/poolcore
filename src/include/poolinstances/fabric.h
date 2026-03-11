#pragma once

#include "poolconfig/config.h"
#include "poolcore/poolCore.h"
#include "poolcore/poolInstance.h"
#include <filesystem>
#include <functional>
#include <unordered_map>

class CPriceFetcher;
class StatisticServer;
class UserManager;

class CInstanceFabric {

public:
  static CPoolInstance *get(asyncBase *base,
                            UserManager &userMgr,
                            const std::vector<PoolBackend*> &linkedBackends,
                            CThreadPool &pool,
                            StatisticServer *algoMetaStatistic,
                            const CInstanceConfig &instanceConfig,
                            unsigned instanceId,
                            unsigned instancesNum,
                            CPriceFetcher *priceFetcher,
                            const std::filesystem::path &logsPath);

private:
  using NewFunction = std::function<CPoolInstance*(
    asyncBase*,
    UserManager&,
    const std::vector<PoolBackend*>&,
    CThreadPool&,
    StatisticServer*,
    const CInstanceConfig&,
    unsigned,
    unsigned,
    CPriceFetcher*,
    const std::filesystem::path&)>;

  static std::unordered_map<std::string, NewFunction> FabricData_;
};
