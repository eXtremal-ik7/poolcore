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

using FCreateNetworkClient = std::function<CNetworkClient*(const std::string&,
                                                            asyncBase*,
                                                            const CCoinInfo&,
                                                            const std::vector<CNodeConfig>&,
                                                            const std::vector<CNodeConfig>&,
                                                            const SelectorByWeight<CMiningAddress>&,
                                                            loguru::LogChannel&)>;

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
  std::vector<FCreateNetworkClient> CreateNetworkClientProcs;
  std::vector<FCreatePoolInstance> CreatePoolInstanceProcs;
};
