#pragma once

#include <atomic>
#include <functional>
#include <memory>

template<typename T>
struct MultiCall {
  std::unique_ptr<T[]> Data;
  std::atomic<size_t> FinishedCallsNum = 0;
  size_t TotalCallsNum;
  std::function<void(const T*, size_t)> MainCallback;

  MultiCall(size_t totalCallsNum, std::function<void(const T*, size_t)> mainCallback) : TotalCallsNum(totalCallsNum), MainCallback(mainCallback) {
    Data.reset(new T[totalCallsNum]);
  }

  std::function<void(const T&)> generateCallback(size_t callNum) {
    return [this, callNum](const T &data) {
      Data[callNum] = data;
      if (++FinishedCallsNum == TotalCallsNum) {
        MainCallback(Data.get(), TotalCallsNum);
        delete this;
      }
    };
  }
};
