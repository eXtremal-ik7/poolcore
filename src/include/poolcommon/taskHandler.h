#pragma once

#include "coroutineJoin.h"
#include "tbb/concurrent_queue.h"
#include "asyncio/asyncio.h"
#include <type_traits>
#include <utility>

struct aioUserEvent;
struct asyncBase;

template<typename ObjectTy>
class Task {
public:
  virtual ~Task() {}
  virtual void run(ObjectTy *object) = 0;
};

template<typename ObjectTy, typename Func>
class CallTask : public Task<ObjectTy> {
public:
  CallTask(Func func) : Func_(std::move(func)) {}
  void run(ObjectTy *object) final { Func_(object); }
private:
  Func Func_;
};

template<typename ObjectTy>
class TaskHandlerCoroutine {
public:
  TaskHandlerCoroutine(ObjectTy *object, asyncBase *base) : Object_(object) {
    TaskQueueEvent_ = newUserEvent(base, 0, nullptr, nullptr);
  }

  void start() {
    coroutineCall(coroutineNewWithCb([](void *arg) { static_cast<TaskHandlerCoroutine*>(arg)->taskHandler(); }, this, 0x100000, coroutineFinishCb, &TaskHandlerFinished));
  }

  void stop(const char *threadName, const char *taskHandlerName) {
    push(nullptr);
    coroutineJoin(threadName, taskHandlerName, &TaskHandlerFinished);
  }

  void push(Task<ObjectTy> *task) {
    TaskQueue_.push(task);
    userEventActivate(TaskQueueEvent_);
  }

  template<typename Func, typename = std::enable_if_t<!std::is_convertible_v<Func, Task<ObjectTy>*>>>
  void push(Func&& func) {
    push(new CallTask<ObjectTy, std::decay_t<Func>>(std::forward<Func>(func)));
  }

private:
  void taskHandler() {
    Task<ObjectTy> *task;
    bool shutdownRequested = false;
    for (;;) {
      while (TaskQueue_.try_pop(task)) {
        std::unique_ptr<Task<ObjectTy>> taskHolder(task);
        if (!task) {
          shutdownRequested = true;
          continue;
        }
        task->run(Object_);
      }

      if (shutdownRequested)
        break;
      ioWaitUserEvent(TaskQueueEvent_);
    }
  }

private:
  ObjectTy *Object_;
  tbb::concurrent_queue<Task<ObjectTy>*> TaskQueue_;
  aioUserEvent *TaskQueueEvent_;

public:
  bool TaskHandlerFinished = false;
};
