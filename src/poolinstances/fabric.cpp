#include "poolinstances/fabric.h"
#include "poolinstances/stratum.h"
#include "poolinstances/zmq.h"

#include "blockmaker/btc.h"
#include "blockmaker/ltc.h"
#include "blockmaker/xpm.h"

std::unordered_map<std::string, PoolInstanceFabric::NewPoolInstanceFunction> PoolInstanceFabric::FabricData_ = {
  {"BTC.stratum", [](asyncBase *base, UserManager &userMgr, CThreadPool &pool, rapidjson::Value &config) { return new StratumInstance<BTC::Proto>(base, userMgr, pool, config); }},
  {"LTC.stratum", [](asyncBase *base, UserManager &userMgr, CThreadPool &pool, rapidjson::Value &config) { return new StratumInstance<LTC::Proto>(base, userMgr, pool, config); }},
  {"XPM.zmq", [](asyncBase *base, UserManager &userMgr, CThreadPool &pool, rapidjson::Value &config) { return new ZmqInstance<XPM::Proto>(base, userMgr, pool, config); }}
};

CPoolInstance *PoolInstanceFabric::get(asyncBase *base, UserManager &userMgr, CThreadPool &pool, const std::string &type, const std::string &protocol, rapidjson::Value &config)
{ 
  std::string instanceId = type + "." + protocol;
  auto It = FabricData_.find(instanceId);
  return It != FabricData_.end() ? It->second(base, userMgr, pool, config) : nullptr;
}
