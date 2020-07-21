#pragma once

#include "common.h"
#include "poolcore/backend.h"
#include "poolcore/poolCore.h"
#include "poolcore/poolInstance.h"
#include "blockmaker/xpm.h"
#include <unordered_map>


template<typename Proto>
class StratumInstance : public CPoolInstance {
public:
  StratumInstance(asyncBase *base, UserManager &userMgr, CThreadPool &threadPool, rapidjson::Value &config) : CPoolInstance(base, userMgr, threadPool) {
    Name_ = (std::string)Proto::TickerName + ".stratum";
    Data_.reset(new ThreadData[threadPool.threadsNum()]);
    if (!(config.HasMember("port") && config["port"].IsUint())) {
      LOG_F(ERROR, "instance %s: can't read 'port' value from config", Name_.c_str());
      exit(1);
    }

    uint16_t port = config["port"].GetUint();
  }

  virtual void checkNewBlockTemplate(rapidjson::Value &blockTemplate, PoolBackend *backend) override {
    intrusive_ptr<CSingleWorkInstance<Proto>> work = ::checkNewBlockTemplate<Proto>(blockTemplate, backend->getConfig(), backend->getCoinInfo(), SerializeBuffer_, Name_);
    if (!work.get())
      return;
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, work));
  }

  virtual void stopWork() override {
    for (unsigned i = 0; i < ThreadPool_.threadsNum(); i++)
      ThreadPool_.startAsyncTask(i, new AcceptWork(*this, nullptr));
  }

  void acceptConnection(unsigned workerId, socketTy socket, HostAddress address) {

  }

  void acceptWork(unsigned workerId, intrusive_ptr<CSingleWorkInstance<Proto>> work) {
    LOG_F(INFO, "Work received for %s", Proto::TickerName);
  }

public:
  class AcceptWork : public CThreadPool::Task {
  public:
    AcceptWork(StratumInstance &instance, intrusive_ptr<CSingleWorkInstance<Proto>> work) : Instance_(instance), Work_(work) {}
    void run(unsigned workerId) final { Instance_.acceptWork(workerId, Work_); }
  private:
    StratumInstance &Instance_;
    intrusive_ptr<CSingleWorkInstance<Proto>> Work_;
  };

private:
  struct ThreadData {
    asyncBase *WorkerBase;
    typename Proto::Block block;
  };

private:
  std::unique_ptr<ThreadData[]> Data_;

  std::string Name_;
  xmstream SerializeBuffer_;
};
