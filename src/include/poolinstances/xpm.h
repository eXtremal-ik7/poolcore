#pragma once

#include "poolcore/poolInstance.h"
#include "blockmaker/xpm.h"
#include <unordered_map>

class XPMWorkInstance : public CWorkInstance {
private:
  XPM::Proto::Block block;
};

class XpmZmqInstance : public CPoolInstance {
public:
  XpmZmqInstance(unsigned workersNum, CPoolThread *workers, const std::string &name, rapidjson::Value &config);

  virtual void stopWork() override;
  virtual void checkNewBlockTemplate(rapidjson::Value &blockTemplate) override;
  virtual void acceptNewConnection(unsigned workerId, aioObject *socket) override;
  virtual void acceptNewWork(unsigned workerId, intrusive_ptr<CWorkInstance> work) override;

private:
  struct ThreadData {
    XPMWorkInstance Work;
  };

private:
  std::string Name_;
  std::unique_ptr<ThreadData[]> Data_;
};
