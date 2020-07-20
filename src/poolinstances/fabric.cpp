#include "poolinstances/fabric.h"
#include "poolinstances/stratum.h"
#include "poolinstances/zmq.h"

#include "blockmaker/btc.h"
#include "blockmaker/ltc.h"
#include "blockmaker/xpm.h"

std::unordered_map<std::string, PoolInstanceFabric::NewPoolInstanceFunction> PoolInstanceFabric::FabricData_ = {
  {"BTC.stratum", [](asyncBase *base, CThreadPool &pool, rapidjson::Value &config) { return new StratumInstance<BTC::Proto>(base, pool, config); }},
  {"LTC.stratum", [](asyncBase *base, CThreadPool &pool, rapidjson::Value &config) { return new StratumInstance<LTC::Proto>(base, pool, config); }},
  {"XPM.zmq", [](asyncBase *base, CThreadPool &pool, rapidjson::Value &config) { return new ZmqInstance<XPM::Proto>(base, pool, config); }}
};

CPoolInstance *PoolInstanceFabric::get(asyncBase *base, CThreadPool &pool, const std::string &type, const std::string &protocol, rapidjson::Value &config)
{ 
  std::string instanceId = type + "." + protocol;
  auto It = FabricData_.find(instanceId);
  return It != FabricData_.end() ? It->second(base, pool, config) : nullptr;
}
