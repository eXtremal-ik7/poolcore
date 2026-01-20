#pragma once

#include "backendData.h"
#include "rapidjson/document.h"
#include <functional>

class PoolBackend;

class ComplexMiningStats {
public:
  virtual ~ComplexMiningStats() {}
  virtual void start() {}
  virtual void onWork(double, PoolBackend*) {}
  virtual void onShare(double, double, const std::vector<PoolBackend*>&, const std::vector<bool>&, const uint256&) {}

  // Asynchronous API
  virtual void query(const rapidjson::Document&, std::function<void(const char*, size_t)> callback) { callback("{}", 2); }
};
