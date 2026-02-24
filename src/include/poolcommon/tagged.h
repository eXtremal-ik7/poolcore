#pragma once

#include "baseBlob.h"
#include "uint.h"
#include "p2putils/xmstream.h"
#include <chrono>
#include <concepts>
#include <cstring>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// ---- Schema concepts ----

template<typename T>
concept HasSchema = requires { { T::schema() } -> std::same_as<decltype(T::schema())>; } &&
                    requires { typename std::integral_constant<int, (T::schema(), 0)>; };

template<typename T> struct OptionalTraits : std::false_type { using inner = T; };
template<typename T> struct OptionalTraits<std::optional<T>> : std::true_type { using inner = T; };

// ---- Field presence tracking ----

struct CFieldPresence {
  uint64_t Bits = 0;
  void set(unsigned tag) { Bits |= (1ull << tag); }
  bool has(unsigned tag) const { return (Bits & (1ull << tag)) != 0; }
};

// ---- CRTP base for has<&T::field>() ----

template<typename Derived>
struct CSerializable {
  CFieldPresence _presence;

  template<auto MemberPtr>
  bool has() const;
};

// ---- Schema field descriptor ----

template<unsigned Tag_, typename T, typename S, T S::*Ptr_>
struct CTaggedField {
  static constexpr unsigned Tag = Tag_;
  static constexpr T S::*Ptr = Ptr_;
  using value_type = T;
  using struct_type = S;

  static const T &get(const S &s) { return s.*Ptr; }
  static T &get(S &s) { return s.*Ptr; }
};

template<typename>
struct MemberPointerTraits;
template<typename T, typename S>
struct MemberPointerTraits<T S::*> {
  using value_type = T;
  using struct_type = S;
};

template<unsigned Tag, auto Ptr>
constexpr auto field() {
  static_assert(Tag > 0 && Tag < 64, "Tag must be in range 1..63");
  using T = typename MemberPointerTraits<decltype(Ptr)>::value_type;
  using S = typename MemberPointerTraits<decltype(Ptr)>::struct_type;
  return CTaggedField<Tag, T, S, Ptr>{};
}

// ---- Tag lookup by member pointer (compile-time) ----

template<auto MemberPtr, typename S>
consteval unsigned tagOf() {
  unsigned result = 0;
  std::apply([&](auto... fields) {
    auto check = [&](auto f) {
      using FieldPtr = std::remove_cv_t<decltype(decltype(f)::Ptr)>;
      using QueryPtr = std::remove_cv_t<decltype(MemberPtr)>;
      if constexpr (std::is_same_v<FieldPtr, QueryPtr>) {
        if (decltype(f)::Ptr == MemberPtr)
          result = decltype(f)::Tag;
      }
    };
    (check(fields), ...);
  }, S::schema());
  return result;
}

template<typename Derived>
template<auto MemberPtr>
bool CSerializable<Derived>::has() const {
  constexpr unsigned tag = tagOf<MemberPtr, Derived>();
  return _presence.has(tag);
}

// ============================================================
//  Wire format:  [tag: uint32] [size: uint64] [data: size bytes]
//  No terminator — boundaries known from size.
//  Vectors of arithmetic: [count: uint32] [val0] [val1] ...
//  Vectors of complex:    [count: uint32] [size0: uint64] [data0] ...
// ============================================================

// ---- Forward declarations ----

template<typename T> requires std::is_arithmetic_v<T>
void writeValue(xmstream &s, const T &v);
inline void writeValue(xmstream &s, const std::string &v);
template<typename T> requires std::is_arithmetic_v<T>
void writeValue(xmstream &s, const std::vector<T> &v);
template<typename T> requires (!std::is_arithmetic_v<T>)
void writeValue(xmstream &s, const std::vector<T> &v);
template<typename T>
void writeValue(xmstream &s, const std::optional<T> &v);
template<HasSchema T>
void writeValue(xmstream &s, const T &obj);
template<typename T> requires std::is_enum_v<T>
void writeValue(xmstream &s, const T &v);
template<typename Rep, typename Period>
void writeValue(xmstream &s, const std::chrono::duration<Rep, Period> &v);
template<unsigned Bits>
void writeValue(xmstream &s, const UInt<Bits> &v);
template<unsigned Bits>
void writeValue(xmstream &s, const BaseBlob<Bits> &v);

template<typename T>
void writeTagged(xmstream &s, unsigned tag, const T &value);

template<typename T> requires std::is_arithmetic_v<T>
bool readValue(xmstream &s, T &v);
inline bool readValue(xmstream &s, std::string &v);
template<typename T> requires std::is_arithmetic_v<T>
bool readValue(xmstream &s, std::vector<T> &v);
template<typename T> requires (!std::is_arithmetic_v<T>)
bool readValue(xmstream &s, std::vector<T> &v);
template<HasSchema T>
bool readValue(xmstream &s, T &obj);
template<typename T> requires std::is_enum_v<T>
bool readValue(xmstream &s, T &v);
template<typename Rep, typename Period>
bool readValue(xmstream &s, std::chrono::duration<Rep, Period> &v);
template<unsigned Bits>
bool readValue(xmstream &s, UInt<Bits> &v);
template<unsigned Bits>
bool readValue(xmstream &s, BaseBlob<Bits> &v);

template<HasSchema S>
bool unserializeFields(xmstream &s, S &obj, size_t boundary);

// ---- Write: tagged field wrapper ----

template<typename T>
void writeTagged(xmstream &s, unsigned tag, const T &value) {
  s.write(static_cast<uint32_t>(tag));
  size_t sizeOffset = s.sizeOf();
  s.write(static_cast<uint64_t>(0));      // size placeholder
  size_t dataStart = s.sizeOf();
  writeValue(s, value);
  uint64_t dataSize = s.sizeOf() - dataStart;
  memcpy(s.data<uint8_t>() + sizeOffset, &dataSize, sizeof(uint64_t));
}

// ---- Write: scalars ----

template<typename T> requires std::is_arithmetic_v<T>
void writeValue(xmstream &s, const T &v) {
  s.write(v);
}

// ---- Write: string ----

inline void writeValue(xmstream &s, const std::string &v) {
  s.write(static_cast<uint32_t>(v.size()));
  if (!v.empty())
    s.write(v.data(), v.size());
}

// ---- Write: enum ----

template<typename T> requires std::is_enum_v<T>
void writeValue(xmstream &s, const T &v) {
  s.write(static_cast<std::underlying_type_t<T>>(v));
}

// ---- Write: chrono::duration ----

template<typename Rep, typename Period>
void writeValue(xmstream &s, const std::chrono::duration<Rep, Period> &v) {
  s.write(static_cast<int64_t>(v.count()));
}

// ---- Write: UInt<Bits> ----

template<unsigned Bits>
void writeValue(xmstream &s, const UInt<Bits> &v) {
  s.write(v.rawData(), v.rawSize());
}

// ---- Write: BaseBlob<Bits> ----

template<unsigned Bits>
void writeValue(xmstream &s, const BaseBlob<Bits> &v) {
  s.write(v.begin(), v.size());
}

// ---- Write: vector of arithmetic (packed) ----

template<typename T> requires std::is_arithmetic_v<T>
void writeValue(xmstream &s, const std::vector<T> &v) {
  s.write(static_cast<uint32_t>(v.size()));
  for (const auto &item : v)
    s.write(item);
}

// ---- Write: vector of complex types (size-prefixed per element) ----

template<typename T> requires (!std::is_arithmetic_v<T>)
void writeValue(xmstream &s, const std::vector<T> &v) {
  s.write(static_cast<uint32_t>(v.size()));
  for (const auto &item : v) {
    size_t sizeOffset = s.sizeOf();
    s.write(static_cast<uint64_t>(0));    // element size placeholder
    size_t dataStart = s.sizeOf();
    writeValue(s, item);
    uint64_t dataSize = s.sizeOf() - dataStart;
    memcpy(s.data<uint8_t>() + sizeOffset, &dataSize, sizeof(uint64_t));
  }
}

// ---- Write: optional (writes inner value, or nothing) ----

template<typename T>
void writeValue(xmstream &s, const std::optional<T> &v) {
  if (v.has_value())
    writeValue(s, *v);
}

// ---- Write: struct with schema ----

template<HasSchema T>
void writeValue(xmstream &s, const T &obj) {
  std::apply([&](auto... fields) {
    auto writeOne = [&](auto f) {
      using FT = typename decltype(f)::value_type;
      if constexpr (OptionalTraits<FT>::value) {
        if (f.get(obj).has_value())
          writeTagged(s, f.Tag, *f.get(obj));
      } else {
        writeTagged(s, f.Tag, f.get(obj));
      }
    };
    (writeOne(fields), ...);
  }, T::schema());
}

// ---- Read: scalars ----

template<typename T> requires std::is_arithmetic_v<T>
bool readValue(xmstream &s, T &v) {
  v = s.read<T>();
  return !s.eof();
}

// ---- Read: string ----

inline bool readValue(xmstream &s, std::string &v) {
  uint32_t len = s.read<uint32_t>();
  if (s.eof())
    return false;
  auto *data = s.seek(len);
  if (!data)
    return false;
  v.assign(reinterpret_cast<const char*>(data), len);
  return true;
}

// ---- Read: enum ----

template<typename T> requires std::is_enum_v<T>
bool readValue(xmstream &s, T &v) {
  auto raw = s.read<std::underlying_type_t<T>>();
  if (s.eof())
    return false;
  v = static_cast<T>(raw);
  return true;
}

// ---- Read: chrono::duration ----

template<typename Rep, typename Period>
bool readValue(xmstream &s, std::chrono::duration<Rep, Period> &v) {
  int64_t count = s.read<int64_t>();
  if (s.eof())
    return false;
  v = std::chrono::duration<Rep, Period>(static_cast<Rep>(count));
  return true;
}

// ---- Read: UInt<Bits> ----

template<unsigned Bits>
bool readValue(xmstream &s, UInt<Bits> &v) {
  s.read(v.rawData(), v.rawSize());
  return !s.eof();
}

// ---- Read: BaseBlob<Bits> ----

template<unsigned Bits>
bool readValue(xmstream &s, BaseBlob<Bits> &v) {
  s.read(v.begin(), v.size());
  return !s.eof();
}

// ---- Read: vector of arithmetic (packed) ----

template<typename T> requires std::is_arithmetic_v<T>
bool readValue(xmstream &s, std::vector<T> &v) {
  uint32_t count = s.read<uint32_t>();
  if (s.eof())
    return false;
  v.resize(count);
  for (uint32_t i = 0; i < count; i++) {
    v[i] = s.read<T>();
    if (s.eof())
      return false;
  }
  return true;
}

// ---- Read: vector of complex types (size-prefixed per element) ----

template<typename T> requires (!std::is_arithmetic_v<T>)
bool readValue(xmstream &s, std::vector<T> &v) {
  uint32_t count = s.read<uint32_t>();
  if (s.eof())
    return false;
  v.resize(count);
  for (uint32_t i = 0; i < count; i++) {
    uint64_t elemSize = s.read<uint64_t>();
    if (s.eof())
      return false;
    if (elemSize > static_cast<uint64_t>(s.remaining()))
      return false;
    const size_t payloadSize = static_cast<size_t>(elemSize);
    size_t elementEnd = s.offsetOf() + payloadSize;
    xmstream elementStream(s.ptr<uint8_t>(), payloadSize);
    bool ok;
    if constexpr (HasSchema<T>) {
      T element{};
      ok = unserializeFields(elementStream, element, payloadSize);
      if (ok)
        v[i] = std::move(element);
    } else {
      ok = readValue(elementStream, v[i]);
    }
    if (!ok || elementStream.offsetOf() != payloadSize)
      return false;
    s.seekSet(elementEnd);
  }
  return true;
}

// ---- Read: struct with schema (top-level, uses remaining bytes) ----

template<HasSchema T>
bool readValue(xmstream &s, T &obj) {
  T parsed{};
  if (!unserializeFields(s, parsed, s.remaining()))
    return false;
  obj = std::move(parsed);
  return true;
}

// ---- Core deserialization: reads tagged fields within a boundary ----

template<HasSchema S>
bool unserializeFields(xmstream &s, S &obj, size_t boundary) {
  if (boundary > s.remaining())
    return false;
  size_t end = s.offsetOf() + boundary;

  if constexpr (requires { obj._presence; }) {
    obj._presence.Bits = 0;
  }

  while (s.offsetOf() + sizeof(uint32_t) + sizeof(uint64_t) <= end) {
    uint32_t tag = s.read<uint32_t>();
    uint64_t size = s.read<uint64_t>();
    if (s.eof())
      return false;
    if (size > static_cast<uint64_t>(end - s.offsetOf()))
      return false;

    const size_t payloadSize = static_cast<size_t>(size);
    size_t fieldEnd = s.offsetOf() + payloadSize;

    bool found = false;
    bool error = false;
    std::apply([&](auto... fields) {
      auto tryRead = [&](auto f) {
        if (found || error || f.Tag != tag)
          return;

        xmstream fieldStream(s.ptr<uint8_t>(), payloadSize);
        using FT = typename decltype(f)::value_type;
        if constexpr (OptionalTraits<FT>::value) {
          using Inner = typename OptionalTraits<FT>::inner;
          Inner val{};
          bool ok;
          if constexpr (HasSchema<Inner>)
            ok = unserializeFields(fieldStream, val, payloadSize);
          else
            ok = readValue(fieldStream, val);
          if (!ok || fieldStream.offsetOf() != payloadSize) {
            error = true;
            return;
          }
          f.get(obj).emplace(std::move(val));
        } else if constexpr (HasSchema<FT>) {
          if (!unserializeFields(fieldStream, f.get(obj), payloadSize) ||
              fieldStream.offsetOf() != payloadSize)
            error = true;
        } else {
          if (!readValue(fieldStream, f.get(obj)) ||
              fieldStream.offsetOf() != payloadSize)
            error = true;
        }
        if (!error)
          found = true;
      };
      (tryRead(fields), ...);
    }, S::schema());

    if (error)
      return false;

    // Track field presence for CSerializable-derived types
    if constexpr (requires { obj._presence; }) {
      if (found)
        obj._presence.set(tag);
    }

    // Always advance to field end (skip unknown data)
    s.seekSet(fieldEnd);
  }

  return s.offsetOf() == end;
}

// ---- Top-level API ----

template<HasSchema T>
void serialize(xmstream &s, const T &obj) {
  writeValue(s, obj);
}

template<HasSchema T>
bool unserialize(xmstream &s, T &obj) {
  T parsed{};
  if (!unserializeFields(s, parsed, s.remaining()))
    return false;
  obj = std::move(parsed);
  return true;
}
