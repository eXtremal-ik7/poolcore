#include "poolinstances/fabric.h"
#include "poolinstances/stratum.h"
#include "poolinstances/zmq.h"

#include "blockmaker/btc.h"
#include "blockmaker/dgb.h"
#include "blockmaker/doge.h"
#include "blockmaker/ltc.h"
#include "blockmaker/xpm.h"

std::unordered_map<std::string, PoolInstanceFabric::NewPoolInstanceFunction> PoolInstanceFabric::FabricData_ = {
  {"BTC.stratum", [](asyncBase *base, UserManager &userMgr, CThreadPool &pool, rapidjson::Value &config) { return new StratumInstance<BTC::X>(base, userMgr, pool, config); }},
  {"DGB.stratum", [](asyncBase *base, UserManager &userMgr, CThreadPool &pool, rapidjson::Value &config) { return new StratumInstance<DGB::X>(base, userMgr, pool, config); }},
  {"DOGE.stratum", [](asyncBase *base, UserManager &userMgr, CThreadPool &pool, rapidjson::Value &config) { return new StratumInstance<DOGE::X>(base, userMgr, pool, config); }},
  {"LTC.stratum", [](asyncBase *base, UserManager &userMgr, CThreadPool &pool, rapidjson::Value &config) { return new StratumInstance<LTC::X>(base, userMgr, pool, config); }},
  {"XPM.zmq", [](asyncBase *base, UserManager &userMgr, CThreadPool &pool, rapidjson::Value &config) { return new ZmqInstance<XPM::X>(base, userMgr, pool, config); }}
};

CPoolInstance *PoolInstanceFabric::get(asyncBase *base, UserManager &userMgr, CThreadPool &pool, const std::string &type, const std::string &protocol, rapidjson::Value &config)
{ 
  std::string instanceId = type + "." + protocol;
  auto It = FabricData_.find(instanceId);
  return It != FabricData_.end() ? It->second(base, userMgr, pool, config) : nullptr;
}
