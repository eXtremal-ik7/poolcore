// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "p2putils/xmstream.h"
#include "xvector.h"

template<typename T> class DynamicPtr;

template<typename T>
T *staticPtr(xmstream &stream, size_t offset) {
  return reinterpret_cast<T*>(stream.data<uint8_t>() + offset);
}

template<typename T>
class DynamicPtr {
private:
  xmstream &Stream_;
  size_t Offset_;

public:
  DynamicPtr(xmstream &stream, size_t offset) : Stream_(stream), Offset_(offset) {}
  xmstream &stream() { return Stream_; }
  size_t offset() { return Offset_; }
  T *ptr() { return staticPtr<T>(Stream_, Offset_); }
  T *operator->() { return ptr(); }
};

template<typename T>
static inline void xvectorFromStream(xmstream &&src, xvector<T> &dst) {
  size_t size = src.sizeOf();
  size_t msize = src.capacity();
  size_t own = src.own();
  dst.set(static_cast<T*>(src.capture()), size, msize, own);
}
