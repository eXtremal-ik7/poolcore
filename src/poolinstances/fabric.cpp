#include "poolinstances/fabric.h"
#include "poolinstances/btc.h"
#include "poolinstances/xpm.h"

std::unordered_map<std::string, PoolInstanceFabric::NewPoolInstanceFunction> PoolInstanceFabric::FabricData_ = {
  {"BTC.stratum", [](unsigned threadsNum, CPoolThread *workers, rapidjson::Value &config) { return new BtcStratumInstance(threadsNum, workers, "BTC.stratum", config, BtcStratumInstance::ESHA256); }},
  {"LTC.stratum", [](unsigned threadsNum, CPoolThread *workers, rapidjson::Value &config) { return new BtcStratumInstance(threadsNum, workers, "LTC.stratum", config, BtcStratumInstance::EScrypt); }},
  {"XPM.zmq", [](unsigned threadsNum, CPoolThread *workers, rapidjson::Value &config) { return new XpmZmqInstance(threadsNum, workers, "XPM.zmq", config); }}
};

CPoolInstance *PoolInstanceFabric::get(unsigned workersNum, CPoolThread *workers, const std::string &type, const std::string &protocol, rapidjson::Value &config)
{ 
  std::string instanceId = type + "." + protocol;
  auto It = FabricData_.find(instanceId);
  return It != FabricData_.end() ? It->second(workersNum, workers, config) : nullptr;
}
