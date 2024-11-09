#include "poolinstances/fabric.h"
#include "poolinstances/stratum.h"
#include "poolinstances/zmq.h"

#include "blockmaker/btc.h"
#include "blockmaker/dgb.h"
#include "blockmaker/doge.h"
#include "blockmaker/eth.h"
#include "blockmaker/ltc.h"
#include "blockmaker/xec.h"
#include "blockmaker/xpm.h"
#include "blockmaker/zec.h"

std::unordered_map<std::string, PoolInstanceFabric::NewPoolInstanceFunction> PoolInstanceFabric::FabricData_ = {
  {"BTC.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new StratumInstance<BTC::X>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config); }},
  {"DGB.qubit.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new StratumInstance<DGB::X<DGB::Algo::EQubit>>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config); }},
  {"DGB.skein.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new StratumInstance<DGB::X<DGB::Algo::ESkein>>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config); }},
  {"DGB.odo.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new StratumInstance<DGB::X<DGB::Algo::EOdo>>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config); }},
  {"DOGE.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new StratumInstance<DOGE::X>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config); }},
  {"ETH.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new StratumInstance<ETH::X>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config); }},
  {"LTC.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new StratumInstance<LTC::X>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config); }},
  {"XEC.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new StratumInstance<XEC::X>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config); }},
  {"ZEC.stratum", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new StratumInstance<ZEC::X>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config); }},
  {"XPM.zmq", [](asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend*> &linkedBackends, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new ZmqInstance<XPM::X>(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config); }}
};

CPoolInstance *PoolInstanceFabric::get(asyncBase *base, UserManager &userMgr, const std::vector<PoolBackend *> &linkedBackends, CThreadPool &pool, const std::string &type, const std::string &protocol, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config)
{ 
  std::string key = type + "." + protocol;
  auto It = FabricData_.find(key);
  return It != FabricData_.end() ? It->second(base, userMgr, linkedBackends, pool, instanceId, instancesNum, config) : nullptr;
}
