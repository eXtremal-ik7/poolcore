#pragma once

#include "poolcommon/intrusive_ptr.h"
#include "asyncio/asyncio.h"
#include "rapidjson/document.h"
#include "tbb/concurrent_queue.h"
#include <atomic>
#include <thread>

class CWorkInstance {
private:
  mutable std::atomic<uintptr_t> Refs_ = 0;
public:
  virtual ~CWorkInstance() {}
  uintptr_t ref_fetch_add(uintptr_t count) const { return Refs_.fetch_add(count); }
  uintptr_t ref_fetch_sub(uintptr_t count) const { return Refs_.fetch_sub(count); }
};

class CPoolInstance {
public:
  CPoolInstance(asyncBase *base) : CommonBase_(base) {}

  // Functions running in listener thread
  /// Send all miners stopping work signal
  virtual void stopWork() = 0;
  /// Function for interact with bitcoin RPC clients
  /// @arg blockTemplate: deserialized 'getblocktemplate' response
  virtual void checkNewBlockTemplate(rapidjson::Value &blockTemplate) = 0;

public:
  // Functions running in worker thread
  virtual void acceptNewConnection(unsigned workerId, aioObject *socket) = 0;
  virtual void acceptNewWork(unsigned workerId, intrusive_ptr<CWorkInstance> work) = 0;

protected:
  // All pool instances shares this base
  asyncBase *CommonBase_;
};

class CPoolThread {
public:
  CPoolThread(unsigned id);
  void start();
  void stop();

  void newConnection(CPoolInstance &instance, aioObject *socket) { startAsyncTask(new AcceptConnectionTask(instance, Id_, socket)); }
  void newWork(CPoolInstance &instance, intrusive_ptr<CWorkInstance> work) { startAsyncTask(new AcceptBlockTemplateTask(instance, Id_, work)); }

private:
  class Task {
  public:
    Task(CPoolInstance &instance, unsigned workerId) : Instance_(instance), WorkerId_(workerId) {}
    virtual void run() = 0;
  protected:
    CPoolInstance &Instance_;
    unsigned WorkerId_;
  };

  class AcceptConnectionTask : public Task {
  public:
    AcceptConnectionTask(CPoolInstance &instance, unsigned workerId, aioObject *socket) : Task(instance, workerId), Socket_(socket) {}
    void run() final { Instance_.acceptNewConnection(WorkerId_, Socket_); }
  private:
    aioObject *Socket_;
  };

  class AcceptBlockTemplateTask : public Task {
  public:
    AcceptBlockTemplateTask(CPoolInstance &instance, unsigned workerId, intrusive_ptr<CWorkInstance> work) : Task(instance, workerId), Work_(work) {}
    void run() final { Instance_.acceptNewWork(WorkerId_, Work_); }
  private:
    intrusive_ptr<CWorkInstance> Work_;
  };

private:
  void startAsyncTask(Task *task) {
    TaskQueue_.push(task);
    userEventActivate(NewTaskEvent_);
  }

  void runTaskQueue();

private:
  // Object/threads owns this base
  unsigned Id_;
  asyncBase *Base_;
  std::thread Thread_;
  tbb::concurrent_queue<Task*> TaskQueue_;
  aioUserEvent *NewTaskEvent_;
};
