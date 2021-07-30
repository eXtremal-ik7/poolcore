#pragma once 

extern "C" {
#include "blockmaker/ethash.h"
}
#include "poolcommon/intrusive_ptr.h"
#include "rapidjson/document.h"
#include <atomic>

struct alignas(512) EthashDagWrapper {
public:
  EthashDagWrapper(unsigned epochNumber, bool bigEpoch) {
    Dag_ = ethashCreateDag(epochNumber, bigEpoch);
  }

  EthashDag *dag() { return Dag_; }

public:
  uintptr_t ref_fetch_add(uintptr_t value) { return Refs_.fetch_add(value); }
  uintptr_t ref_fetch_sub(uintptr_t value) { return Refs_.fetch_sub(value); }

private:
  EthashDag *Dag_ = nullptr;
  std::atomic<uintptr_t> Refs_ = 0;
};

class CBlockTemplate {
private:
  mutable std::atomic<uintptr_t> Refs_ = 0;
public:
  // TODO: Don't send json document to stratum workers
  rapidjson::Document Document;
  uint64_t UniqueWorkId;
  double Difficulty;
  intrusive_ptr<EthashDagWrapper> DagFile;
  uintptr_t ref_fetch_add(uintptr_t count) const { return Refs_.fetch_add(count); }
  uintptr_t ref_fetch_sub(uintptr_t count) const { return Refs_.fetch_sub(count); }
};
