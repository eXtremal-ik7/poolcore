#include "poolinstances/fabric.h"
#include "poolinstances/stratum.h"
#include "poolinstances/zmq.h"

#include "blockmaker/btc.h"
#include "blockmaker/dgb.h"
#include "blockmaker/doge.h"
#include "blockmaker/ltc.h"
#include "blockmaker/xpm.h"

std::unordered_map<std::string, PoolInstanceFabric::NewPoolInstanceFunction> PoolInstanceFabric::FabricData_ = {
  {"BTC.stratum", [](asyncBase *base, UserManager &userMgr, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new StratumInstance<BTC::X>(base, userMgr, pool, instanceId, instancesNum, config); }},
  {"DGB.stratum", [](asyncBase *base, UserManager &userMgr, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new StratumInstance<DGB::X>(base, userMgr, pool, instanceId, instancesNum, config); }},
  {"DOGE.stratum", [](asyncBase *base, UserManager &userMgr, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new StratumInstance<DOGE::X>(base, userMgr, pool, instanceId, instancesNum, config); }},
  {"LTC.stratum", [](asyncBase *base, UserManager &userMgr, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new StratumInstance<LTC::X>(base, userMgr, pool, instanceId, instancesNum, config); }},
  {"XPM.zmq", [](asyncBase *base, UserManager &userMgr, CThreadPool &pool, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config) { return new ZmqInstance<XPM::X>(base, userMgr, pool, instanceId, instancesNum, config); }}
};

CPoolInstance *PoolInstanceFabric::get(asyncBase *base, UserManager &userMgr, CThreadPool &pool, const std::string &type, const std::string &protocol, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config)
{ 
  std::string key = type + "." + protocol;
  auto It = FabricData_.find(key);
  return It != FabricData_.end() ? It->second(base, userMgr, pool, instanceId, instancesNum, config) : nullptr;
}
