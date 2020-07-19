#include "poolcore/poolInstance.h"
#include "poolcore/thread.h"
#include "loguru.hpp"

void CPoolInstance::stopWork()
{
  for (unsigned i = 0; i < WorkersNum_; i++)
    Workers_[i].stopWork(*this);
}

CPoolThread::CPoolThread()
{
  Base_ = createAsyncBase(amOSDefault);
  NewTaskEvent_ = newUserEvent(Base_, 0, [](aioUserEvent*, void *arg) {
    static_cast<CPoolThread*>(arg)->runTaskQueue();
  }, this);
}

void CPoolThread::start(unsigned id)
{
  Id_ = id;
  Thread_ = std::thread([](CPoolThread *object) {
    char name[32];
    snprintf(name, sizeof(name), "worker%u", object->Id_);
    loguru::set_thread_name(name);
    InitializeWorkerThread();
    LOG_F(INFO, "worker %u started tid=%u", object->Id_, GetWorkerThreadId());
    asyncLoop(object->Base_);
  }, this);
}

void CPoolThread::stop()
{
  postQuitOperation(Base_);
  Thread_.join();
  LOG_F(INFO, "worker %u stopped", Id_);
}

void CPoolThread::runTaskQueue()
{
  Task *t;
  std::unique_ptr<Task> task;
  while (TaskQueue_.try_pop(t)) {
    task.reset(t);
    t->run();
  }
}
