#pragma once

#include "p2putils/xmstream.h"
#include <string>

template<class T, typename Enable=void>
struct is_simple_numeric : std::integral_constant<bool,
        std::is_same<T, int8_t>::value ||
        std::is_same<T, uint8_t>::value ||
        std::is_same<T, int16_t>::value ||
        std::is_same<T, uint16_t>::value ||
        std::is_same<T, int32_t>::value ||
        std::is_same<T, uint32_t>::value ||
        std::is_same<T, int64_t>::value ||
        std::is_same<T, uint64_t>::value> {};

struct VarSize {
  uint64_t Size;
  VarSize() {}
  VarSize(uint64_t size) : Size(size) {}
};

template<typename T, typename Enable=void>
struct DbKeyIo {
  static void serialize(xmstream &out, const T &data);
  static void unserialize(xmstream &in, T &data);
};

template<typename T, typename Enable=void>
struct DbIo {
  static void serialize(xmstream &out, const T &data);
  static void unserialize(xmstream &in, T &data);
};

// Serialization for simple integer types
template<typename T>
struct DbIo<T, typename std::enable_if<is_simple_numeric<T>::value, void>::type> {
  static inline void serialize(xmstream &stream, const T &data) { stream.writele<T>(data); }
  static inline void unserialize(xmstream &stream, T &data) { data = stream.readle<T>(); }
};

// variable size
template<> struct DbIo<VarSize> {
  static inline void serialize(xmstream &out, const VarSize &data) {
    if (data.Size < 0xFD) {
      out.write<uint8_t>(static_cast<uint8_t>(data.Size));
    } else if (data.Size <= 0xFFFF) {
      out.write<uint8_t>(0xFD);
      out.writele<uint16_t>(static_cast<uint16_t>(data.Size));
    } else if (data.Size <= 0xFFFFFFFF) {
      out.write<uint8_t>(0xFE);
      out.writele<uint32_t>(static_cast<uint32_t>(data.Size));
    } else {
      out.write<uint8_t>(0xFF);
      out.writele<uint64_t>(data.Size);
    }
  }
  static inline void unserialize(xmstream &in, VarSize &data) {
    uint8_t type = in.read<uint8_t>();
    if (type < 0xFD)
      data.Size = type;
    else if (type == 0xFD)
      data.Size = in.readle<uint16_t>();
    else if (type == 0xFE)
      data.Size = in.readle<uint32_t>();
    else
      data.Size = in.readle<uint64_t>();
  }
};

// string
// Serialization for std::string
template<> struct DbIo<std::string> {
  static inline void serialize(xmstream &dst, const std::string &data) {
    DbIo<VarSize>::serialize(dst, data.size());
    dst.write(data.data(), data.size());
  }
  static inline void unserialize(xmstream &src, std::string &data) {
    VarSize length;
    DbIo<VarSize>::unserialize(src, length);
    data.assign(src.seek<const char>(length.Size), length.Size);
  }
};
