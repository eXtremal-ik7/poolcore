#include "poolinstances/fabric.h"
#include "poolinstances/stratum.h"
#include "poolinstances/zmq.h"

#include "blockmaker/btc.h"
#include "blockmaker/dash.h"
#include "blockmaker/dgb.h"
#include "blockmaker/doge.h"
#include "blockmaker/eth.h"
#include "blockmaker/ltc.h"
#include "blockmaker/xpm.h"
#include "blockmaker/zec.h"

std::unordered_map<std::string, CInstanceFabric::NewFunction> CInstanceFabric::FabricData_ = {
  {"BTC.stratum", [](asyncBase *base,
                     UserManager &userMgr,
                     const std::vector<PoolBackend*> &linkedBackends,
                     CThreadPool &pool,
                     StatisticServer *algoMetaStatistic,
                     ComplexMiningStats *miningStats,
                     const CInstanceConfig &instanceConfig,
                     unsigned instanceId,
                     unsigned instancesNum,
                     CPriceFetcher *priceFetcher,
                     const std::filesystem::path &logsPath) {
    return new StratumInstance<BTC::X>(
      base, userMgr, linkedBackends, pool, algoMetaStatistic, miningStats, instanceId, instancesNum, instanceConfig, priceFetcher, logsPath);
  }},
  {"DASH.stratum", [](asyncBase *base,
                      UserManager &userMgr,
                      const std::vector<PoolBackend*> &linkedBackends,
                      CThreadPool &pool,
                      StatisticServer *algoMetaStatistic,
                      ComplexMiningStats *miningStats,
                      const CInstanceConfig &instanceConfig,
                      unsigned instanceId,
                      unsigned instancesNum,
                      CPriceFetcher *priceFetcher,
                      const std::filesystem::path &logsPath) {
    return new StratumInstance<DASH::X>(
      base, userMgr, linkedBackends, pool, algoMetaStatistic, miningStats, instanceId, instancesNum, instanceConfig, priceFetcher, logsPath);
  }},
  {"DGB.qubit.stratum", [](asyncBase *base,
                           UserManager &userMgr,
                           const std::vector<PoolBackend*> &linkedBackends,
                           CThreadPool &pool,
                           StatisticServer *algoMetaStatistic,
                           ComplexMiningStats *miningStats,
                           const CInstanceConfig &instanceConfig,
                           unsigned instanceId,
                           unsigned instancesNum,
                           CPriceFetcher *priceFetcher,
                           const std::filesystem::path &logsPath) {
    return new StratumInstance<DGB::X<DGB::Algo::EQubit>>(
      base, userMgr, linkedBackends, pool, algoMetaStatistic, miningStats, instanceId, instancesNum, instanceConfig, priceFetcher, logsPath);
  }},
  {"DGB.skein.stratum", [](asyncBase *base,
                           UserManager &userMgr,
                           const std::vector<PoolBackend*> &linkedBackends,
                           CThreadPool &pool,
                           StatisticServer *algoMetaStatistic,
                           ComplexMiningStats *miningStats,
                           const CInstanceConfig &instanceConfig,
                           unsigned instanceId,
                           unsigned instancesNum,
                           CPriceFetcher *priceFetcher,
                           const std::filesystem::path &logsPath) {
    return new StratumInstance<DGB::X<DGB::Algo::ESkein>>(
      base, userMgr, linkedBackends, pool, algoMetaStatistic, miningStats, instanceId, instancesNum, instanceConfig, priceFetcher, logsPath);
  }},
  {"DGB.odo.stratum", [](asyncBase *base,
                         UserManager &userMgr,
                         const std::vector<PoolBackend*> &linkedBackends,
                         CThreadPool &pool,
                         StatisticServer *algoMetaStatistic,
                         ComplexMiningStats *miningStats,
                         const CInstanceConfig &instanceConfig,
                         unsigned instanceId,
                         unsigned instancesNum,
                         CPriceFetcher *priceFetcher,
                         const std::filesystem::path &logsPath) {
    return new StratumInstance<DGB::X<DGB::Algo::EOdo>>(
      base, userMgr, linkedBackends, pool, algoMetaStatistic, miningStats, instanceId, instancesNum, instanceConfig, priceFetcher, logsPath);
  }},
  {"DOGE.stratum", [](asyncBase *base,
                      UserManager &userMgr,
                      const std::vector<PoolBackend*> &linkedBackends,
                      CThreadPool &pool,
                      StatisticServer *algoMetaStatistic,
                      ComplexMiningStats *miningStats,
                      const CInstanceConfig &instanceConfig,
                      unsigned instanceId,
                      unsigned instancesNum,
                      CPriceFetcher *priceFetcher,
                      const std::filesystem::path &logsPath) {
    return new StratumInstance<DOGE::X>(
      base, userMgr, linkedBackends, pool, algoMetaStatistic, miningStats, instanceId, instancesNum, instanceConfig, priceFetcher, logsPath);
  }},
  {"ETH.stratum", [](asyncBase *base,
                     UserManager &userMgr,
                     const std::vector<PoolBackend*> &linkedBackends,
                     CThreadPool &pool,
                     StatisticServer *algoMetaStatistic,
                     ComplexMiningStats *miningStats,
                     const CInstanceConfig &instanceConfig,
                     unsigned instanceId,
                     unsigned instancesNum,
                     CPriceFetcher *priceFetcher,
                     const std::filesystem::path &logsPath) {
    return new StratumInstance<ETH::X>(
      base, userMgr, linkedBackends, pool, algoMetaStatistic, miningStats, instanceId, instancesNum, instanceConfig, priceFetcher, logsPath);
  }},
  {"LTC.stratum", [](asyncBase *base,
                     UserManager &userMgr,
                     const std::vector<PoolBackend*> &linkedBackends,
                     CThreadPool &pool,
                     StatisticServer *algoMetaStatistic,
                     ComplexMiningStats *miningStats,
                     const CInstanceConfig &instanceConfig,
                     unsigned instanceId,
                     unsigned instancesNum,
                     CPriceFetcher *priceFetcher,
                     const std::filesystem::path &logsPath) {
    return new StratumInstance<LTC::X>(
      base, userMgr, linkedBackends, pool, algoMetaStatistic, miningStats, instanceId, instancesNum, instanceConfig, priceFetcher, logsPath);
  }},
  {"ZEC.stratum", [](asyncBase *base,
                     UserManager &userMgr,
                     const std::vector<PoolBackend*> &linkedBackends,
                     CThreadPool &pool,
                     StatisticServer *algoMetaStatistic,
                     ComplexMiningStats *miningStats,
                     const CInstanceConfig &instanceConfig,
                     unsigned instanceId,
                     unsigned instancesNum,
                     CPriceFetcher *priceFetcher,
                     const std::filesystem::path &logsPath) {
    return new StratumInstance<ZEC::X>(
      base, userMgr, linkedBackends, pool, algoMetaStatistic, miningStats, instanceId, instancesNum, instanceConfig, priceFetcher, logsPath);
  }},
  {"XPM.zmq", [](asyncBase *base,
                 UserManager &userMgr,
                 const std::vector<PoolBackend*> &linkedBackends,
                 CThreadPool &pool,
                 StatisticServer *algoMetaStatistic,
                 ComplexMiningStats *miningStats,
                 const CInstanceConfig &instanceConfig,
                 unsigned instanceId,
                 unsigned instancesNum,
                 CPriceFetcher*,
                 const std::filesystem::path &logsPath) {
    return new ZmqInstance<XPM::X>(
      base, userMgr, linkedBackends, pool, algoMetaStatistic, miningStats, instanceId, instancesNum, instanceConfig, logsPath);
  }}
};

CPoolInstance *CInstanceFabric::get(asyncBase *base,
                                    UserManager &userMgr,
                                    const std::vector<PoolBackend*> &linkedBackends,
                                    CThreadPool &pool,
                                    StatisticServer *algoMetaStatistic,
                                    ComplexMiningStats *miningStats,
                                    const CInstanceConfig &instanceConfig,
                                    unsigned instanceId,
                                    unsigned instancesNum,
                                    CPriceFetcher *priceFetcher,
                                    const std::filesystem::path &logsPath)
{
  std::string key = instanceConfig.Type + "." + instanceConfig.Protocol;
  auto It = FabricData_.find(key);
  return It != FabricData_.end() ?
    It->second(base, userMgr, linkedBackends, pool, algoMetaStatistic, miningStats, instanceConfig, instanceId, instancesNum, priceFetcher, logsPath) :
    nullptr;
}
