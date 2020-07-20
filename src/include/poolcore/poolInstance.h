#pragma once

#include "poolcommon/intrusive_ptr.h"
#include "asyncio/asyncio.h"
#include "rapidjson/document.h"
#include "tbb/concurrent_queue.h"
#include <atomic>
#include <thread>

class PoolBackend;

class CWorkInstance {
private:
  mutable std::atomic<uintptr_t> Refs_ = 0;
public:
  virtual ~CWorkInstance() {}
  uintptr_t ref_fetch_add(uintptr_t count) const { return Refs_.fetch_add(count); }
  uintptr_t ref_fetch_sub(uintptr_t count) const { return Refs_.fetch_sub(count); }
};

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
    virtual void run(unsigned workerId) = 0;
  };

public:
  void startAsyncTask(unsigned workerId, Task *task) {
    Threads_[workerId].TaskQueue.push(task);
    userEventActivate(Threads_[workerId].NewTaskEvent);
  }

private:
  struct ThreadData {
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
  CPoolInstance(asyncBase *base, CThreadPool &threadPool) : MonitorBase_(base), ThreadPool_(threadPool) {}

  // Functions running in listener thread
  /// Send all miners stopping work signal
  virtual void stopWork() = 0;
  /// Function for interact with bitcoin RPC clients
  /// @arg blockTemplate: deserialized 'getblocktemplate' response
  virtual void checkNewBlockTemplate(rapidjson::Value &blockTemplate, PoolBackend *backend) = 0;

protected:
  asyncBase *MonitorBase_;
  CThreadPool &ThreadPool_;
};
