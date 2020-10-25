#include "poolcore/poolInstance.h"
#include "poolcore/thread.h"
#include "loguru.hpp"


CThreadPool::CThreadPool(unsigned threadsNum) : ThreadsNum_(threadsNum)
{
  Threads_.reset(new ThreadData[threadsNum]);
  for (unsigned i = 0; i < threadsNum; i++) {
    ThreadData &threadData = Threads_[i];
    threadData.Id = i;
    threadData.Base = createAsyncBase(amOSDefault);
    // Temporary use "semaphore" event
    threadData.NewTaskEvent = newUserEvent(threadData.Base, 1, [](aioUserEvent*, void *arg) {
      runTaskQueue(*static_cast<ThreadData*>(arg));
    }, &threadData);
  }
}

void CThreadPool::start()
{
  for (unsigned i = 0; i < ThreadsNum_; i++)  {
    ThreadData &threadData = Threads_[i];
    threadData.Thread = std::thread([](ThreadData *threadData) {
      char name[32];
      srand(static_cast<unsigned>(time(nullptr))*threadData->Id);
      snprintf(name, sizeof(name), "worker%u", threadData->Id);
      loguru::set_thread_name(name);
      InitializeWorkerThread();
      SetLocalThreadId(threadData->Id);
      LOG_F(INFO, "worker %u started tid=%u", threadData->Id, GetGlobalThreadId());
      asyncLoop(threadData->Base);
    }, &threadData);
  }
}

void CThreadPool::stop()
{
  for (unsigned i = 0; i < ThreadsNum_; i++)  {
    ThreadData &threadData = Threads_[i];
    postQuitOperation(threadData.Base);
    threadData.Thread.join();
    LOG_F(INFO, "worker %u stopped", threadData.Id);
  }
}

void CThreadPool::runTaskQueue(ThreadData &data)
{
  Task *t;
  std::unique_ptr<Task> task;
  while (data.TaskQueue.try_pop(t)) {
    task.reset(t);
    t->run(data.Id);
  }
}
