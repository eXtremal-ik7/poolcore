#pragma once

#include "p2putils/xmstream.h"
#include <cstring>
#include <string>

namespace JSON {

static inline char bin2hexLowerCaseDigit(uint8_t b, bool upperCase)
{
  return b < 10 ? '0'+b : (upperCase ? 'A' : 'a') + b - 10;
}

static inline void writeEscapedString(xmstream &stream, const char *data, size_t size)
{
  const char *start = data;
  const char *end = data + size;
  for (const char *p = start; p < end; ++p) {
    uint8_t ch = static_cast<uint8_t>(*p);
    if (ch >= 0x20 && ch != '"' && ch != '\\')
      continue;
    if (p > start)
      stream.write(start, p - start);
    switch (ch) {
      case '"':  stream.write("\\\"", 2); break;
      case '\\': stream.write("\\\\", 2); break;
      case '\b': stream.write("\\b", 2); break;
      case '\f': stream.write("\\f", 2); break;
      case '\n': stream.write("\\n", 2); break;
      case '\r': stream.write("\\r", 2); break;
      case '\t': stream.write("\\t", 2); break;
      default: {
        char buf[6] = {
          '\\', 'u', '0', '0',
          bin2hexLowerCaseDigit(ch >> 4, false),
          bin2hexLowerCaseDigit(ch & 0xF, false)
        };
        stream.write(buf, 6);
        break;
      }
    }
    start = p + 1;
  }
  if (end > start)
    stream.write(start, end - start);
}

class Object {
public:
  Object(xmstream &stream) : Stream_(stream) { stream.write('{'); }
  ~Object() {
    Stream_.write('}');
    Stream_.truncate();
  }

  template<typename T>
  void addInt(const char *name, T number) {
    addField(name);
    char *out = Stream_.reserve<char>(24);
    size_t size = xitoa(number, out);
    Stream_.seek(size-24);
  }

  template<typename T>
  void addIntHex(const char *name, T number, bool leadingZeroes = true, bool zeroxPrefix = false, bool upperCase = false) {
    addField(name);
    Stream_.write('\"');
    char *out = Stream_.reserve<char>(24);
    size_t size = xitoah(number, out, leadingZeroes, zeroxPrefix, upperCase);
    Stream_.seek(size-24);
    Stream_.write('\"');
  }

  void addDouble(const char *name, double number) {
    addField(name);

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.12g", number);
    Stream_.write(static_cast<const char*>(buffer));
  }

  void addString(const char *name, const std::string &value) {
    addField(name);
    Stream_.write('\"');
    writeEscapedString(Stream_, value.data(), value.size());
    Stream_.write('\"');
  }

  void addString(const char *name, const void *value, size_t size) {
    addField(name);
    Stream_.write('\"');
    writeEscapedString(Stream_, static_cast<const char*>(value), size);
    Stream_.write('\"');
  }

  void addString(const char *name, const char *value) {
    addField(name);
    Stream_.write('\"');
    writeEscapedString(Stream_, value, strlen(value));
    Stream_.write('\"');
  }

  void addHex(const char *name, const void *data, size_t size, bool zeroxPrefix = false, bool upperCase = false) {
    addField(name);
    Stream_.write('\"');
    if (zeroxPrefix)
      Stream_.write("0x");

    char *out = Stream_.reserve<char>(size*2);
    const uint8_t *pIn = static_cast<const uint8_t*>(data);
    for (size_t i = 0, ie = size; i != ie; ++i) {
      out[i*2] = bin2hexLowerCaseDigit(pIn[i] >> 4, upperCase);
      out[i*2+1] = bin2hexLowerCaseDigit(pIn[i] & 0xF, upperCase);
    }

    Stream_.write('\"');
  }

  void addBoolean(const char *name, bool value) {
    addField(name);
    Stream_.write(value ? "true" : "false");
  }

  void addNull(const char *name) {
    addField(name);
    Stream_.write("null");
  }

  void addCustom(const char *name, const char *value) {
    addField(name);
    Stream_.write(value);
  }

  void addCustom(const char *name, const std::string &value) {
    addField(name);
    Stream_.write(value.data(), value.size());
  }

  void addField(const char *name) {
    if (HasFields_)
      Stream_.write(',');
    HasFields_ = true;

    Stream_.write('\"');
    Stream_.write(name);
    Stream_.write("\":");
  }

private:
  xmstream &Stream_;
  bool HasFields_ = false;
};

class Array {
public:
  Array(xmstream &stream) : Stream_(stream) { stream.write('['); }
  ~Array() {
    Stream_.write(']');
    Stream_.truncate();
  }

  template<typename T>
  void addInt(T number) {
    addField();
    char *out = Stream_.reserve<char>(24);
    size_t size = xitoa(number, out);
    Stream_.seek(size-24);
  }

  template<typename T>
  void addIntHex(T number, bool leadingZeroes = true, bool zeroxPrefix = false, bool upperCase = false) {
    addField();
    Stream_.write('\"');
    char *out = Stream_.reserve<char>(24);
    size_t size = xitoah(number, out, leadingZeroes, zeroxPrefix, upperCase);
    Stream_.seek(size-24);
    Stream_.write('\"');
  }

  void addDouble(double number) {
    addField();

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.12g", number);
    Stream_.write(static_cast<const char*>(buffer));
  }

  void addString(const void *value, size_t size) {
    addField();
    Stream_.write('\"');
    writeEscapedString(Stream_, static_cast<const char*>(value), size);
    Stream_.write('\"');
  }

  void addString(const std::string &value) {
    addField();
    Stream_.write('\"');
    writeEscapedString(Stream_, value.data(), value.size());
    Stream_.write('\"');
  }

  void addString(const char *value) {
    addField();
    Stream_.write('\"');
    writeEscapedString(Stream_, value, strlen(value));
    Stream_.write('\"');
  }

  void addHex(const void *data, size_t size, bool zeroxPrefix = false, bool upperCase = false) {
    addField();
    Stream_.write('\"');
    if (zeroxPrefix)
      Stream_.write("0x");

    char *out = Stream_.reserve<char>(size*2);
    const uint8_t *pIn = static_cast<const uint8_t*>(data);
    for (size_t i = 0, ie = size; i != ie; ++i) {
      out[i*2] = bin2hexLowerCaseDigit(pIn[i] >> 4, upperCase);
      out[i*2+1] = bin2hexLowerCaseDigit(pIn[i] & 0xF, upperCase);
    }

    Stream_.write('\"');
  }

  void addBoolean(bool value) {
    addField();
    Stream_.write(value ? "true" : "false");
  }

  void addNull() {
    addField();
    Stream_.write("null");
  }

  void addCustom(const char *value) {
    addField();
    Stream_.write(value);
  }

  void addCustom(const std::string &value) {
    addField();
    Stream_.write(value.data(), value.size());
  }

  void addField() {
    if (HasFields_)
      Stream_.write(',');
    HasFields_ = true;
  }

private:
  xmstream &Stream_;
  bool HasFields_ = false;
};

}
