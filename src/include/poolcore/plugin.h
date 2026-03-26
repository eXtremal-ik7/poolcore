#pragma once
#include "poolconfig/config.h"
#include "poolcore/miningAddress.h"
#include "poolcore/poolCore.h"
#include "poolcore/poolInstance.h"
#include "poolcore/priceFetcher.h"
#include <filesystem>
#include <functional>

// Plugin callbacks
using FAddExtraCoin = std::function<bool(const char*, CCoinInfo&)>;
using FAddExtraCoinOld2 = std::function<bool(const char*, CCoinInfoOld2&)>;

using FAddRpcClient = std::function<CNetworkClient*(const std::string&,
                                                    asyncBase*,
                                                    const CCoinInfo&,
                                                    const CNodeConfig&,
                                                    const SelectorByWeight<CMiningAddress>&)>;

using FAddRpcClientForTerminal = std::function<CNetworkClient*(const std::string&,
                                                               asyncBase*,
                                                               const CCoinInfo&,
                                                               const CNodeConfig&,
                                                               const SelectorByWeight<CMiningAddress>&,
                                                               const std::string&,
                                                               const std::vector<std::string>&,
                                                               const std::vector<std::string>&)>;

using FCreatePoolInstance = std::function<CPoolInstance*(asyncBase*,
                                                         UserManager&,
                                                         const std::vector<PoolBackend*>&,
                                                         CThreadPool&,
                                                         StatisticServer*,
                                                         const CInstanceConfig&,
                                                         unsigned,
                                                         unsigned,
                                                         CPriceFetcher*,
                                                         const std::filesystem::path&)>;

struct CPluginContext {
  std::vector<FAddExtraCoin> AddExtraCoinProcs;
  std::vector<FAddExtraCoinOld2> AddExtraCoinOld2Procs;
  std::vector<FAddRpcClient> AddRpcClientProcs;
  std::vector<FAddRpcClientForTerminal> AddRpcClientForTerminalProcs;
  std::vector<FCreatePoolInstance> CreatePoolInstanceProcs;
};
