#pragma once
#include "poolcommon/baseBlob.h"
#include "idltool/jsonReadError.h"
#include "idltool/jsonReadString.h"
#include "idltool/jsonWriteString.h"

// hash256: BaseBlob<256> as 64-char LE hex string (Bitcoin display order)
inline JsonReadError __hash256Parse(const char *&p, const char *end, BaseBlob<256> &out) {
  std::string value;
  auto error = jsonReadStringValue(p, end, value);
  if (error != JsonReadError::Ok)
    return error;
  if (value.size() != 64)
    return JsonReadError::UnexpectedChar;
  for (char c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return JsonReadError::UnexpectedChar;
  }
  out.setHexLE(value.c_str());
  return JsonReadError::Ok;
}

inline void __hash256Serialize(std::string &out, const BaseBlob<256> &value) {
  size_t pos = out.size();
  out.resize(pos + 66);
  char *p = out.data() + pos;
  p[0] = '"';
  value.getHexLE(p + 1);
  p[65] = '"';
}

// hash256x: BaseBlob<256> as "0x" + 64-char LE hex string (Ethereum display order)
inline JsonReadError __hash256xParse(const char *&p, const char *end, BaseBlob<256> &out) {
  std::string value;
  auto error = jsonReadStringValue(p, end, value);
  if (error != JsonReadError::Ok)
    return error;
  if (value.size() != 66 || value[0] != '0' || value[1] != 'x')
    return JsonReadError::UnexpectedChar;
  for (size_t i = 2; i < 66; i++) {
    char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return JsonReadError::UnexpectedChar;
  }
  out.setHexLE(value.c_str() + 2);
  return JsonReadError::Ok;
}

inline void __hash256xSerialize(std::string &out, const BaseBlob<256> &value) {
  size_t pos = out.size();
  out.resize(pos + 68);
  char *p = out.data() + pos;
  p[0] = '"';
  p[1] = '0';
  p[2] = 'x';
  value.getHexLE(p + 3);
  p[67] = '"';
}

// hash256be: BaseBlob<256> as 64-char BE hex string (raw byte order)
inline JsonReadError __hash256beParse(const char *&p, const char *end, BaseBlob<256> &out) {
  std::string value;
  auto error = jsonReadStringValue(p, end, value);
  if (error != JsonReadError::Ok)
    return error;
  if (value.size() != 64)
    return JsonReadError::UnexpectedChar;
  for (char c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return JsonReadError::UnexpectedChar;
  }
  out.setHexRaw(value.c_str());
  return JsonReadError::Ok;
}

inline void __hash256beSerialize(std::string &out, const BaseBlob<256> &value) {
  size_t pos = out.size();
  out.resize(pos + 66);
  char *p = out.data() + pos;
  p[0] = '"';
  value.getHexRaw(p + 1);
  p[65] = '"';
}
