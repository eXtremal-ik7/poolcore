#include "poolinstances/fabric.h"
#include "poolinstances/stratum.h"
#include "poolinstances/zmq.h"

#include "blockmaker/btc.h"
#include "blockmaker/ltc.h"
#include "blockmaker/xpm.h"

std::unordered_map<std::string, PoolInstanceFabric::NewPoolInstanceFunction> PoolInstanceFabric::FabricData_ = {
  {"BTC.stratum", [](unsigned threadsNum, CPoolThread *workers, rapidjson::Value &config) { return new StratumInstance<BTC::Proto>(threadsNum, workers, config); }},
  {"LTC.stratum", [](unsigned threadsNum, CPoolThread *workers, rapidjson::Value &config) { return new StratumInstance<LTC::Proto>(threadsNum, workers, config); }},
  {"XPM.zmq", [](unsigned threadsNum, CPoolThread *workers, rapidjson::Value &config) { return new ZmqInstance<XPM::Proto>(threadsNum, workers, config); }}
};

CPoolInstance *PoolInstanceFabric::get(unsigned workersNum, CPoolThread *workers, const std::string &type, const std::string &protocol, rapidjson::Value &config)
{ 
  std::string instanceId = type + "." + protocol;
  auto It = FabricData_.find(instanceId);
  return It != FabricData_.end() ? It->second(workersNum, workers, config) : nullptr;
}
