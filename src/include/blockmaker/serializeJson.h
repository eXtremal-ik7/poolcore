// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once 

#include "xvector.h"
#include "poolcommon/uint256.h"
#include <p2putils/xmstream.h>
#include <p2putils/strExtras.h>

static inline char hexDigit(uint8_t N)
{
  if (N <= 9)
    return '0' + N;
  else
    return 'a' + N - 10;
}

template<typename T>
static inline void serializeJsonInt(xmstream &stream, const char *fieldName, T data) {
  char N[32];
  xitoa(data, N);
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }
  stream.write(N, strlen(N));
}

static inline void serializeJson(xmstream &stream, const char *fieldName, int32_t data) { serializeJsonInt(stream, fieldName, data); }
static inline void serializeJson(xmstream &stream, const char *fieldName, int64_t data) { serializeJsonInt(stream, fieldName, data); }
static inline void serializeJson(xmstream &stream, const char *fieldName, uint32_t data) { serializeJsonInt(stream, fieldName, data); }
static inline void serializeJson(xmstream &stream, const char *fieldName, uint64_t data) { serializeJsonInt(stream, fieldName, data); }

static inline void serializeJson(xmstream &stream, const char *fieldName, const std::string &data) {
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }
  stream.write('\"');
  stream.write(data.data(), data.size());
  stream.write('\"');
}

static inline void serializeJson(xmstream &stream, const char *fieldName, const uint256 &data) {
  std::string hex = data.GetHex();
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }
  stream.write('\"');
  stream.write(hex.data(), hex.size());
  stream.write('\"');
}

template<typename T> static inline void serializeJson(xmstream &stream, const char *fieldName, const xvector<T> &data) {
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }
  stream.write('[');
  for (size_t i = 0, ie = data.size(); i < ie; i++) {
    if (i != 0)
      stream.write(',');
    serializeJson(stream, nullptr, data[i]);
  }
  stream.write(']');
}

// special case: vector<uint8_t>
static inline void serializeJson(xmstream &stream, const char *fieldName, const xvector<uint8_t> &data)  {
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }

  stream.write('\"');
  char *out = stream.reserve<char>(data.size()*2);
  for (size_t i = 0, ie = data.size(); i < ie; i++) {
    out[i*2+0] = hexDigit(data[i] >> 4);
    out[i*2+1] = hexDigit(data[i] & 0x0F);
  }
  stream.write('\"');
}

template<typename T>
static inline void serializeJson(xmstream &stream, const char *fieldName, std::vector<T> &data) {
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }
  stream.write('[');
  for (size_t i = 0, ie = data.size(); i < ie; i++) {
    if (i != 0)
      stream.write(',');
    serializeJson(stream, nullptr, data[i]);
  }
  stream.write(']');
}
