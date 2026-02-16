#pragma once

#include "poolcommon/intrusive_ptr.h"
#include "poolcore/complexMiningStats.h"
#include "asyncio/asyncio.h"
#include "tbb/concurrent_queue.h"
#include <atomic>
#include <thread>
#include <vector>

class CBlockTemplate;
class PoolBackend;
class StatisticServer;
class UserManager;

class CThreadPool {
public:
  CThreadPool(unsigned threadsNum);
  unsigned threadsNum() { return ThreadsNum_; }
  asyncBase *getBase(unsigned workerId) { return Threads_[workerId].Base; }
  void start();
  void stop();

public:
  class Task {
  public:
    Task() {}
    virtual ~Task() {}
    virtual void run(unsigned workerId) = 0;
  };

public:
  void startAsyncTask(unsigned workerId, Task *task) {
    Threads_[workerId].TaskQueue.push(task);
    userEventActivate(Threads_[workerId].NewTaskEvent);
  }

private:
  struct ThreadData {
    ThreadData() {}
    ThreadData(const ThreadData&) = delete;
    ThreadData& operator=(const ThreadData&) = delete;

    asyncBase *Base;
    unsigned Id;
    std::thread Thread;
    tbb::concurrent_queue<Task*> TaskQueue;
    aioUserEvent *NewTaskEvent;
  };

private:
  static void runTaskQueue(ThreadData &data);

private:
  unsigned ThreadsNum_;
  std::unique_ptr<ThreadData[]> Threads_;
};

class CPoolInstance {
public:
  CPoolInstance(asyncBase *base,
                UserManager &userMgr,
                const std::vector<PoolBackend*> &linkedBackends,
                CThreadPool &threadPool,
                StatisticServer *algoMetaStatistic,
                ComplexMiningStats *miningStats)
      : MonitorBase_(base),
        UserMgr_(userMgr),
        LinkedBackends_(linkedBackends),
        ThreadPool_(threadPool),
        AlgoMetaStatistic_(algoMetaStatistic),
        MiningStats_(miningStats) {}

  virtual ~CPoolInstance() {}

  // Functions running in listener thread
  /// Start timers and network listeners (must be called after full initialization)
  virtual void start() = 0;
  /// Send all miners stopping work signal
  virtual void stopWork() = 0;
  /// Function for interact with bitcoin RPC clients
  /// @arg blockTemplate: deserialized 'getblocktemplate' response
  virtual void checkNewBlockTemplate(CBlockTemplate *blockTemplate, PoolBackend *backend) = 0;

protected:
  asyncBase *MonitorBase_;
  UserManager &UserMgr_;
  std::vector<PoolBackend*> LinkedBackends_;
  CThreadPool &ThreadPool_;
  StatisticServer *AlgoMetaStatistic_ = nullptr;
  ComplexMiningStats *MiningStats_ = nullptr;
};
