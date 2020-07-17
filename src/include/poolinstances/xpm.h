#pragma once

#include "poolcore/poolInstance.h"
#include "blockmaker/xpm.h"
#include <unordered_map>

class XPMWorkInstance : public CWorkInstance {
  XPM::Proto::Block block;
};

class XpmZmqInstance : public CPoolInstance {
public:
  XpmZmqInstance(asyncBase *base, rapidjson::Value &config) : CPoolInstance(base) {}

  virtual void checkNewBlockTemplate(const rapidjson::Document &blockTemplate) override;
  virtual void acceptNewConnection(unsigned workerId, aioObject *socket) override;
  virtual void acceptNewWork(unsigned workerId, intrusive_ptr<CWorkInstance> work) override
  ;
private:
  struct ThreadData {
    XPMWorkInstance Work;
  };

private:
  std::vector<ThreadData> Data_;
};
