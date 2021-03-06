#pragma once

#include "blockmaker/serializeUtils.h"
#include "blockmaker/xvector.h"
#include "poolcommon/uint256.h"
#include "p2putils/xmstream.h"
#include <string>

namespace BTC {

template<typename T, typename Enable=void>
struct Io {
  static inline void serialize(xmstream &src, const T &data);
  static inline void unserialize(xmstream &dst, T &data);
  static inline void unpack(xmstream &src, DynamicPtr<T> dst);
  static inline void unpackFinalize(DynamicPtr<T> dst);
};

template<typename T> static inline void serialize(xmstream &src, const T &data) { Io<T>::serialize(src, data); }
template<typename T> static inline void unserialize(xmstream &dst, T &data) { Io<T>::unserialize(dst, data); }
template<typename T> static inline void unpack(xmstream &src, DynamicPtr<T> dst) { Io<T>::unpack(src, dst); }
template<typename T> static inline void unpackFinalize(DynamicPtr<T> dst) { Io<T>::unpackFinalize(dst); }

static inline void serializeForCoinbase(xmstream &stream, int64_t value)
{
  size_t offset = stream.offsetOf();
  stream.write<uint8_t>(0);
  if (value == 0)
    return;

  bool isNegative = value < 0;
  uint64_t absValue = isNegative ? -value : value;
  while (absValue >= 0x100)  {
    stream.write<uint8_t>(absValue & 0xFF);
    absValue >>= 8;
  }

  if (absValue & 0x80) {
    stream.write<uint8_t>(static_cast<uint8_t>(absValue));
    stream.write<uint8_t>(isNegative ? 0x80 : 0);
  } else {
    stream.write<uint8_t>(static_cast<uint8_t>(absValue | (isNegative ? 0x80 : 0)));
  }

  stream.data<uint8_t>()[offset] = static_cast<uint8_t>(stream.offsetOf() - offset - 1);
}

// variable size
static inline void serializeVarSize(xmstream &stream, uint64_t value)
{
  if (value < 0xFD) {
    stream.write<uint8_t>(static_cast<uint8_t>(value));
  } else if (value <= 0xFFFF) {
    stream.write<uint8_t>(0xFD);
    stream.writele<uint16_t>(static_cast<uint16_t>(value));
  } else if (value <= 0xFFFFFFFF) {
    stream.write<uint8_t>(0xFE);
    stream.writele<uint32_t>(static_cast<uint32_t>(value));
  } else {
    stream.write<uint8_t>(0xFF);
    stream.writele<uint64_t>(value);
  }
}

static inline void unserializeVarSize(xmstream &stream, uint64_t &out)
{
  uint8_t type = stream.read<uint8_t>();
  if (type < 0xFD)
    out = type;
  else if (type == 0xFD)
    out = stream.readle<uint16_t>();
  else if (type == 0xFE)
    out = stream.readle<uint32_t>();
  else
    out = stream.readle<uint64_t>();
}

static inline size_t serializedVarSizeLength(uint64_t value)
{
  if (value < 0xFD) {
    return 1;
  } else if (value <= 0xFFFF) {
    return 3;
  } else if (value <= 0xFFFFFFFF) {
    return 5;
  } else {
    return 9;
  }
}

}

namespace BTC {
// TODO: use C++20 and concepts
template<class T>
struct is_simple_numeric : std::integral_constant<bool,
        std::is_same<T, int8_t>::value ||
        std::is_same<T, uint8_t>::value ||
        std::is_same<T, int16_t>::value ||
        std::is_same<T, uint16_t>::value ||
        std::is_same<T, int32_t>::value ||
        std::is_same<T, uint32_t>::value ||
        std::is_same<T, int64_t>::value ||
        std::is_same<T, uint64_t>::value> {};

// Serialization for simple integer types
template<typename T>
struct Io<T, typename std::enable_if<is_simple_numeric<T>::value, void>::type> {
  static inline void serialize(xmstream &stream, const T &data) { stream.writele<T>(data); }
  static inline void unserialize(xmstream &stream, T &data) { data = stream.readle<T>(); }
};

// Serialization for bool
template<> struct Io<bool> {
  static inline void serialize(xmstream &stream, const bool &data) { stream.writele(static_cast<uint8_t>(data)); }
  static inline void unserialize(xmstream &stream, bool &data) { data = stream.readle<uint8_t>(); }
};

// Serialization for base_blob (including uint256) types
template<unsigned int BITS> struct Io<base_blob<BITS>> {
  static inline void serialize(xmstream &stream, const base_blob<BITS> &data) { stream.write(data.begin(), data.size()); }
  static inline void unserialize(xmstream &stream, base_blob<BITS> &data) { stream.read(data.begin(), data.size()); }
};

// string
// Serialization for std::string
// NOTE: unpacking not supported
template<> struct Io<std::string> {
  static inline void serialize(xmstream &dst, const std::string &data) {
    serializeVarSize(dst, data.size());
    dst.write(data.data(), data.size());
  }
  static inline void unserialize(xmstream &src, std::string &data) {
    uint64_t length;
    unserializeVarSize(src, length);
    data.assign(src.seek<const char>(length), length);
  }
};

// array
template<size_t Size> struct Io<std::array<uint8_t, Size>> {
  static inline void serialize(xmstream &dst, const std::array<uint8_t, Size> &data) {
    dst.write(data.data(), Size);
  }

  static inline void unserialize(xmstream &src, std::array<uint8_t, Size> &data) {
    src.read(data.data(), Size);
  }
};

// xvector
template<typename T> struct Io<xvector<T>> {
  static inline void serialize(xmstream &dst, const xvector<T> &data) {
    serializeVarSize(dst, data.size());
    for (const auto &v: data)
      BTC::serialize(dst, v);
  }

  static inline void unserialize(xmstream &src, xvector<T> &data) {
    uint64_t size = 0;
    unserializeVarSize(src, size);
    if (size > src.remaining()) {
      src.seekEnd(0, true);
      return;
    }

    data.resize(size);
    for (uint64_t i = 0; i < size; i++)
      BTC::unserialize(src, data[i]);
  }

  static inline void unpack(xmstream &src, DynamicPtr<xvector<T>> dst) {
    uint64_t size;
    unserializeVarSize(src, size);

    size_t dataOffset = dst.stream().offsetOf();
    dst.stream().reserve(size*sizeof(T));

    new (dst.ptr()) xvector<T>(reinterpret_cast<T*>(dataOffset), size, false);
    for (uint64_t i = 0; i < size; i++)
      BTC::unpack(src, DynamicPtr<T>(dst.stream(), dataOffset + sizeof(T)*i));
  }

  static inline void unpackFinalize(DynamicPtr<xvector<T>> dst) {
    xvector<T> *ptr = dst.ptr();

    // Change offset to absolute address for xvector data
    size_t size = ptr->size();
    size_t dataOffset = reinterpret_cast<size_t>(ptr->data());
    new (ptr) xvector<T>(reinterpret_cast<T*>(dst.stream().template data<uint8_t>() + dataOffset), size);

    // finalize unpacking for all vector elements
    for (size_t i = 0; i < size; i++)
      BTC::unpackFinalize(DynamicPtr<T>(dst.stream(), dataOffset + sizeof(T)*i));
  }
};

// Context-dependend xvector
template<typename T, typename ContextTy> struct Io<xvector<T>, ContextTy> {
  static inline void serialize(xmstream &dst, const xvector<T> &data, ContextTy context) {
    serializeVarSize(dst, data.size());
    for (const auto &v: data)
      Io<T, ContextTy>::serialize(dst, v, context);
  }

  static inline void unserialize(xmstream &src, xvector<T> &data, ContextTy context) {
    uint64_t size = 0;
    unserializeVarSize(src, size);
    if (size > src.remaining()) {
      src.seekEnd(0, true);
      return;
    }

    data.resize(size);
    for (uint64_t i = 0; i < size; i++)
      Io<T, ContextTy>::unserialize(src, data[i], context);
  }
};

// Special case: xvector<uint8_t>
template<> struct Io<xvector<uint8_t>> {
  static inline void serialize(xmstream &dst, const xvector<uint8_t> &data) {
    serializeVarSize(dst, data.size());
    dst.write(data.data(), data.size());
  }

  static inline void unserialize(xmstream &src, xvector<uint8_t> &data) {
    uint64_t size = 0;
    unserializeVarSize(src, size);
    if (size > src.remaining()) {
      src.seekEnd(0, true);
      return;
    }

    data.resize(size);
    src.read(data.data(), size);
  }

  static inline void unpack(xmstream &src, DynamicPtr<xvector<uint8_t>> dst) {
    uint64_t size;
    unserializeVarSize(src, size);

    size_t dataOffset = dst.stream().offsetOf();
    void *data = src.seek(size);
    if (data)
      dst.stream().write(data, size);

    new (dst.ptr()) xvector<uint8_t>(reinterpret_cast<uint8_t*>(dataOffset), size);
  }

  static inline void unpackFinalize(DynamicPtr<xvector<uint8_t>> dst) {
    xvector<uint8_t> *ptr = dst.ptr();

    // Change offset to absolute address for xvector data
    size_t size = ptr->size();
    size_t dataOffset = reinterpret_cast<size_t>(ptr->data());
    new (ptr) xvector<uint8_t>(dst.stream().data<uint8_t>() + dataOffset, size);
  }
};

template<typename T>
static inline bool unpack(xmstream &src, xmstream &dst) {
  size_t offset = dst.offsetOf();
  dst.reserve(sizeof(T));
  DynamicPtr<T> ptr(dst, offset);
  BTC::Io<T>::unpack(src, ptr);
  if (src.eof())
    return false;
  BTC::Io<T>::unpackFinalize(ptr);
  return true;
}

// unserialize & check
template<typename T>
static inline bool unserializeAndCheck(xmstream &stream, T &data) {
  BTC::Io<T>::unserialize(stream, data);
  return !stream.eof();
}

}
