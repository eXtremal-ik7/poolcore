#pragma once

#include "poolcore/poolInstance.h"
#include "blockmaker/btc.h"
#include <unordered_map>

class BTCWorkInstance : public CWorkInstance {
  BTC::Proto::Block block;
};

class BtcStratumInstance : public CPoolInstance {
public:
  enum EPowAlgorithm {
    ESHA256,
    EScrypt
  };

public:
  BtcStratumInstance(asyncBase *base, rapidjson::Value &config, EPowAlgorithm algo) : CPoolInstance(base) {}

  virtual void stopWork() override;
  virtual void checkNewBlockTemplate(rapidjson::Value &blockTemplate) override;
  virtual void acceptNewConnection(unsigned workerId, aioObject *socket) override;
  virtual void acceptNewWork(unsigned workerId, intrusive_ptr<CWorkInstance> work) override;

private:
  struct ThreadData {
    BTCWorkInstance Work;
  };

private:
  std::vector<ThreadData> Data_;
};
