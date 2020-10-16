#pragma once

#include "poolcommon/uint256.h"
#include "p2putils/xmstream.h"
#include <list>
#include <string>
#include <vector>

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
};

template<typename T, typename Enable=void>
struct DbIo {
  static void serialize(xmstream &out, const T &data);
  static void unserialize(xmstream &in, T &data);
};

// Serialization for simple integer types
template<typename T>
struct DbKeyIo<T, typename std::enable_if<is_simple_numeric<T>::value, void>::type> {
  static inline void serialize(xmstream &stream, const T &data) { stream.writebe<T>(data); }
};

template<typename T>
struct DbIo<T, typename std::enable_if<is_simple_numeric<T>::value, void>::type> {
  static inline void serialize(xmstream &stream, const T &data) { stream.writele<T>(data); }
  static inline void unserialize(xmstream &stream, T &data) { data = stream.readle<T>(); }
};

template<>
struct DbIo<bool> {
  static inline void serialize(xmstream &stream, const bool &data) { stream.write<uint8_t>(data); }
  static inline void unserialize(xmstream &stream, bool &data) { data = stream.read<uint8_t>(); }
};

template<>
struct DbIo<double> {
  static inline void serialize(xmstream &stream, const double data) { stream.write<double>(data); }
  static inline void unserialize(xmstream &stream, double &data) { data = stream.read<double>(); }
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
template<> struct DbKeyIo<std::string> {
  static inline void serialize(xmstream &dst, const std::string &data) {
    DbKeyIo<uint32_t>::serialize(dst, static_cast<uint32_t>(data.size()));
    dst.write(data.data(), data.size());
  }
};

template<> struct DbIo<std::string> {
  static inline void serialize(xmstream &dst, const std::string &data) {
    DbIo<VarSize>::serialize(dst, data.size());
    dst.write(data.data(), data.size());
  }
  static inline void unserialize(xmstream &src, std::string &data) {
    VarSize length;
    DbIo<VarSize>::unserialize(src, length);
    if (length.Size <= 32*1048576)
      data.assign(src.seek<const char>(length.Size), length.Size);
    else
      src.seekEnd(0, true);
  }
};

template<> struct DbIo<uint256> {
  static inline void serialize(xmstream &dst, const uint256 &data) {
    dst.write(data.begin(), data.size());
  }

  static inline void unserialize(xmstream &src, uint256 &data) {
    src.read(data.begin(), data.size());
  }
};

template<> struct DbIo<uint512> {
  static inline void serialize(xmstream &dst, const uint512 &data) {
    dst.write(data.begin(), data.size());
  }

  static inline void unserialize(xmstream &src, uint512 &data) {
    src.read(data.begin(), data.size());
  }
};

template<> struct DbKeyIo<uint512> {
  // TODO: check it
  static inline void serialize(xmstream &dst, const uint512 &data) {
    dst.write(data.begin(), data.size());
  }
};

// std::vector
template<typename T> struct DbIo<std::vector<T>> {
  static inline void serialize(xmstream &dst, const std::vector<T> &data) {
    DbIo<VarSize>::serialize(dst, data.size());
    for (const auto &v: data)
      DbIo<T>::serialize(dst, v);
  }

  static inline void unserialize(xmstream &src, std::vector<T> &data) {
    VarSize size = 0;
    DbIo<VarSize>::unserialize(src, size);
    if (size.Size > src.remaining()) {
      src.seekEnd(0, true);
      return;
    }

    data.resize(size.Size);
    for (uint64_t i = 0; i < size.Size; i++)
      DbIo<T>::unserialize(src, data[i]);
  }
};

// std::list
template<typename T> struct DbIo<std::list<T>> {
  static inline void serialize(xmstream &dst, const std::list<T> &data) {
    DbIo<VarSize>::serialize(dst, data.size());
    for (const auto &v: data)
      DbIo<T>::serialize(dst, v);
  }

  static inline void unserialize(xmstream &src, std::list<T> &data) {
    VarSize size = 0;
    DbIo<VarSize>::unserialize(src, size);
    if (size.Size > src.remaining()) {
      src.seekEnd(0, true);
      return;
    }

    for (uint64_t i = 0; i < size.Size; i++) {
      T &element = data.emplace_back();
      DbIo<T>::unserialize(src, element);
    }
  }
};
