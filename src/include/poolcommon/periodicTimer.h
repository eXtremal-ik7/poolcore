#pragma once

#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "loguru.hpp"
#include <chrono>
#include <functional>
#include <thread>

class CPeriodicTimer {
public:
  CPeriodicTimer(asyncBase *base) {
    Event_ = newUserEvent(base, 1, nullptr, nullptr);
  }

  // Event-only mode: fires only on activate()
  void start(std::function<void()> callback, unsigned stackSize = 0x100000) {
    Callback_ = std::move(callback);
    EventOnly_ = true;
    coroutineCall(coroutineNewWithCb([](void *arg) {
      static_cast<CPeriodicTimer*>(arg)->handler();
    }, this, stackSize, finishCb, &Finished_));
  }

  // Periodic mode: fires every interval (and on activate())
  void start(std::function<void()> callback, std::chrono::microseconds interval, bool immediate = false, bool lastCallOnStop = false, unsigned stackSize = 0x100000) {
    Callback_ = std::move(callback);
    IntervalUs_ = interval.count();
    Immediate_ = immediate;
    LastCallOnStop_ = lastCallOnStop;
    coroutineCall(coroutineNewWithCb([](void *arg) {
      static_cast<CPeriodicTimer*>(arg)->handler();
    }, this, stackSize, finishCb, &Finished_));
  }

  // Signal shutdown (non-blocking)
  void stop() {
    ShutdownRequested_ = true;
    userEventActivate(Event_);
  }

  // Wait for coroutine to finish (blocking)
  void wait(const char *threadName, const char *timerName) {
    CLOG_F(INFO, "{}: {} finishing", threadName, timerName);
    while (!Finished_)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  void setInterval(std::chrono::microseconds interval) {
    IntervalUs_ = interval.count();
  }

  void pause() {
    Paused_ = true;
    userEventActivate(Event_);
  }

  void resume() {
    Paused_ = false;
    userEventActivate(Event_);
  }

  void activate() {
    userEventActivate(Event_);
  }

private:
  static void finishCb(void *arg) {
    *static_cast<bool*>(arg) = true;
  }

  void handler() {
    if (Immediate_)
      Callback_();
    for (;;) {
      if (Paused_ || EventOnly_)
        ioWaitUserEvent(Event_);
      else
        ioSleep(Event_, IntervalUs_);
      if (ShutdownRequested_)
        break;
      if (!Paused_)
        Callback_();
    }
    if (LastCallOnStop_)
      Callback_();
  }

  aioUserEvent *Event_;
  std::function<void()> Callback_;
  int64_t IntervalUs_ = 0;
  bool Immediate_ = false;
  bool LastCallOnStop_ = false;
  bool EventOnly_ = false;
  bool Paused_ = false;
  bool ShutdownRequested_ = false;
  bool Finished_ = false;
};
