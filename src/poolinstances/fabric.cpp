#include "poolinstances/fabric.h"
#include "poolinstances/stratum.h"
#include "poolinstances/zmq.h"

#include "blockmaker/btc.h"
#include "blockmaker/dgb.h"
#include "blockmaker/doge.h"
#include "blockmaker/eth.h"
#include "blockmaker/ltc.h"
#include "blockmaker/xpm.h"
#include "blockmaker/zec.h"

std::unordered_map<std::string, PoolInstanceFabric::NewPoolInstanceFunction> PoolInstanceFabric::FabricData_ = {
  {"BTC.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config, CPriceFetcher *priceFetcher) { return new StratumInstance<BTC::X>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config, priceFetcher); }},
  {"DGB.qubit.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config, CPriceFetcher *priceFetcher) { return new StratumInstance<DGB::X<DGB::Algo::EQubit>>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config, priceFetcher); }},
  {"DGB.skein.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config, CPriceFetcher *priceFetcher) { return new StratumInstance<DGB::X<DGB::Algo::ESkein>>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config, priceFetcher); }},
  {"DGB.odo.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config, CPriceFetcher *priceFetcher) { return new StratumInstance<DGB::X<DGB::Algo::EOdo>>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config, priceFetcher); }},
  {"DOGE.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config, CPriceFetcher *priceFetcher) { return new StratumInstance<DOGE::X>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config, priceFetcher); }},
  {"ETH.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config, CPriceFetcher *priceFetcher) { return new StratumInstance<ETH::X>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config, priceFetcher); }},
  {"LTC.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config, CPriceFetcher *priceFetcher) { return new StratumInstance<LTC::X>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config, priceFetcher); }},
  {"ZEC.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config, CPriceFetcher *priceFetcher) { return new StratumInstance<ZEC::X>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config, priceFetcher); }},
  {"XPM.zmq", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config, CPriceFetcher*) { return new ZmqInstance<XPM::X>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config); }}
};

CPoolInstance *PoolInstanceFabric::get(asyncBase *base,
                                      UserManager &userMgr,
                                      const std::vector<PoolBackend *> &linkedBackends,
                                      CThreadPool &pool,
                                      const std::string &type,
                                      const std::string &protocol,
                                      unsigned instanceId,
                                      unsigned instancesNum,
                                      rapidjson::Value &config,
                                      CPriceFetcher *priceFetcher)
{ 
  std::string key = type + "." + protocol;
  auto It = FabricData_.find(key);
  return It != FabricData_.end() ? It->second(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config, priceFetcher) : nullptr;
}
