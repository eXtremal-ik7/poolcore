#pragma once

#include "poolcore/poolCore.h"
#include "poolcore/poolInstance.h"
#include "rapidjson/document.h"
#include <functional>
#include <unordered_map>

class UserManager;

class PoolInstanceFabric {
public:
  static CPoolInstance *get(asyncBase *base, UserManager &userMgr, CThreadPool &pool, const std::string &type, const std::string &protocol, unsigned instanceId, unsigned instancesNum, rapidjson::Value &config);

private:
  using NewPoolInstanceFunction = std::function<CPoolInstance*(asyncBase*, UserManager&, CThreadPool&, unsigned, unsigned, rapidjson::Value&)>;

private:
  static std::unordered_map<std::string, NewPoolInstanceFunction> FabricData_;
};
