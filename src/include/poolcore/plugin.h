#pragma once
#include "poolcore/backendData.h"
#include "poolcore/complexMiningStats.h"
#include "poolcore/poolCore.h"
#include "poolcore/poolInstance.h"
#include "poolcore/priceFetcher.h"
#include <functional>

// Plugin callbacks
using FAddExtraCoin = std::function<bool(const char*, CCoinInfo&)>;
using FAddExtraCoinOld2 = std::function<bool(const char*, CCoinInfoOld2&)>;

using FAddRpcClient = std::function<CNetworkClient*(const std::string&,
                                                    asyncBase*,
                                                    unsigned,
                                                    const CCoinInfo&,
                                                    const CNodeConfig&,
                                                    const PoolBackendConfig&)>;

using FAddRpcClientForTerminal = std::function<CNetworkClient*(const std::string&,
                                                               asyncBase*,
                                                               unsigned,
                                                               const CCoinInfo&,
                                                               const CNodeConfig&,
                                                               const PoolBackendConfig&,
                                                               const std::string&,
                                                               const std::vector<std::string>&,
                                                               const std::vector<std::string>&)>;

using FCreatePoolInstance = std::function<CPoolInstance*(asyncBase*,
                                                         UserManager&,
                                                         const std::vector<PoolBackend*>&,
                                                         CThreadPool&,
                                                         StatisticServer*,
                                                         ComplexMiningStats*,
                                                         const std::string&,
                                                         const std::string&,
                                                         unsigned,
                                                         unsigned,
                                                         rapidjson::Value&,
                                                         CPriceFetcher*,
                                                         const std::filesystem::path&)>;

using FCreateMiningStatsHandler = std::function<ComplexMiningStats*(const std::vector<std::unique_ptr<PoolBackend>>&,
                                                                    const std::filesystem::path&)>;

struct CPluginContext {
  bool HasMiningStatsHandler = false;
  std::vector<FAddExtraCoin> AddExtraCoinProcs;
  std::vector<FAddExtraCoinOld2> AddExtraCoinOld2Procs;
  std::vector<FAddRpcClient> AddRpcClientProcs;
  std::vector<FAddRpcClientForTerminal> AddRpcClientForTerminalProcs;
  std::vector<FCreatePoolInstance> CreatePoolInstanceProcs;
  FCreateMiningStatsHandler CreateMiningStatsHandlerProc;
};
