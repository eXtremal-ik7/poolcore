#include "poolinstances/fabric.h"
#include "poolinstances/btc.h"
#include "poolinstances/xpm.h"

std::unordered_map<std::string, PoolInstanceFabric::NewPoolInstanceFunction> PoolInstanceFabric::FabricData_ = {
  {"BTC.stratum", [](asyncBase *base, rapidjson::Value &config) { return new BtcStratumInstance(base, config, BtcStratumInstance::ESHA256); }},
  {"LTC.stratum", [](asyncBase *base, rapidjson::Value &config) { return new BtcStratumInstance(base, config, BtcStratumInstance::EScrypt); }},
  {"XPM.zmq", [](asyncBase *base, rapidjson::Value &config) { return new XpmZmqInstance(base, config); }}
};

CPoolInstance *PoolInstanceFabric::get(asyncBase *base, const std::string &type, const std::string &protocol, rapidjson::Value &config)
{ 
  std::string instanceId = type + "." + protocol;
  auto It = FabricData_.find(instanceId);
  return It != FabricData_.end() ? It->second(base, config) : nullptr;
}
