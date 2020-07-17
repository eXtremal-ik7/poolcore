#include "poolcore/poolInstance.h"

CPoolThread::CPoolThread(unsigned id) :
  Id_(id)
{
  Base_ = createAsyncBase(amOSDefault);
  NewTaskEvent_ = newUserEvent(Base_, 0, [](aioUserEvent*, void *arg) {
    static_cast<CPoolThread*>(arg)->runTaskQueue();
  }, this);
}

void CPoolThread::start()
{
  Thread_ = std::thread([](CPoolThread *object) {
    asyncLoop(object->Base_);
  }, this);
}

void CPoolThread::stop()
{
  postQuitOperation(Base_);
  Thread_.join();
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
