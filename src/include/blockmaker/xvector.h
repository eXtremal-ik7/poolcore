// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <utility>

template<typename T>
class xvector {
private:
  T *Data_;
  size_t Size_;
  size_t MemorySize_;
  bool Own_;

  void grow() {
    size_t newMemorySize_ = MemorySize_ ? MemorySize_*2 : 1;
    T *newData = static_cast<T*>(operator new(sizeof(T)*newMemorySize_));
    for (size_t i = 0; i < MemorySize_; i++)
      new(&newData[i]) T(Data_[i]);

    free();
    Data_ = newData;
    MemorySize_ = newMemorySize_;
    Own_ = true;
  }

  void grow(size_t newSize) {
    size_t newMemorySize_ = MemorySize_ ? MemorySize_*2 : 1;
    while (newMemorySize_ < newSize)
      newMemorySize_ *= 2;

    T *newData = static_cast<T*>(operator new(sizeof(T)*newMemorySize_));
    for (size_t i = 0; i < MemorySize_; i++)
      new(&newData[i]) T(Data_[i]);

    free();
    Data_ = newData;
    MemorySize_ = newMemorySize_;
    Own_ = true;
  }

  void free() {
    if (Own_) {
      for (size_t i = 0; i < Size_; i++)
        Data_[i].~T();
      operator delete(Data_);
    }
  }

public:
  xvector() : Data_(nullptr), Size_(0), MemorySize_(0), Own_(false) {}

  explicit xvector(const xvector<T> &data) : Own_(true) {
    Size_ = data.Size_;
    MemorySize_ = data.Size_;
    Data_ = static_cast<T*>(operator new(sizeof(T) * Size_));
    for (size_t i = 0; i < Size_; i++)
      new (&Data_[i]) T(data.Data_[i]);
  }

  constexpr xvector& operator=(const xvector<T> &data) {
    Size_ = data.Size_;
    MemorySize_ = data.Size_;
    Own_ = true;
    Data_ = static_cast<T*>(operator new(sizeof(T) * Size_));
    for (size_t i = 0; i < Size_; i++)
      new (&Data_[i]) T(data.Data_[i]);
    return *this;
  }

  explicit xvector(xvector<T> &&data) {
    Data_ = data.Data_;
    Size_ = data.Size_;
    MemorySize_ = data.MemorySize_;
    Own_ = data.Own_;
    data.Data_ = nullptr;
    data.MemorySize_ = 0;
    data.Own_ = false;
  }

  xvector(std::initializer_list<T> list) : Own_(true) {
    Data_ = static_cast<T*>(operator new(sizeof(T)*list.size()));
    for (size_t i = 0; i < list.size(); i++)
      new (&Data_[i]) T(*(list.begin() + i));
    Size_ = list.size();
    MemorySize_ = list.size();
  }

  xvector(T *data, size_t size, bool own=false): Data_(data), Size_(size), MemorySize_(size), Own_(own) {}
  ~xvector() { free(); }
  
  void set(T *data, size_t size, size_t memorySize, bool own=false) {
    Data_ = data;
    Size_ = size;
    MemorySize_ = memorySize;
    Own_ = own;
  }

  T *data() const { return Data_; }
  size_t size() const { return Size_; }
  bool empty() const { return Size_ == 0; }

  T &operator[](size_t index) const { return Data_[index]; }
  T &front() const { return Data_[0]; }
  T &back() const { return Data_[Size_-1]; }
  T *begin() const { return Data_; }
  T *end() const { return Data_ + Size_; }
  
  template<typename... Ts> void emplace_back(Ts&&... args) {
    if (Size_ >= MemorySize_)
      grow();
    new(&Data_[Size_++]) T(std::forward<Ts>(args)...);
  }

  void resize(size_t size) {
    if (Size_ < size)
      grow(size);

    for (size_t i = Size_; i < size; i++)
      new(&Data_[i]) T();

    Size_ = size;
  }
};
