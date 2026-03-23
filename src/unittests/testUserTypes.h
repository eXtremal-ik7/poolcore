#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include "idltool/jsonReadError.h"
#include "idltool/jsonReadString.h"
#include "idltool/jsonReadUInt.h"
#include "idltool/jsonWriteString.h"
#include "idltool/jsonWriteUInt.h"

// plainVal: int64_t as decimal string
inline JsonReadError __plainValParse(const char *&p, const char *end, int64_t &out) {
  std::string raw;
  auto error = jsonReadStringValue(p, end, raw);
  if (error != JsonReadError::Ok)
    return error;
  char *endp;
  out = strtoll(raw.c_str(), &endp, 10);
  if (endp == raw.c_str() || *endp != '\0')
    return JsonReadError::UnexpectedChar;
  return JsonReadError::Ok;
}

inline void __plainValSerialize(xmstream &out, int64_t value) {
  jsonWriteString(out, std::to_string(value));
}

// counterVal: uint64_t as uint64 (identity)
inline JsonReadError __counterValParse(const char *&p, const char *end, uint64_t &out) {
  return jsonReadUInt64(p, end, out);
}

inline void __counterValSerialize(xmstream &out, uint64_t value) {
  jsonWriteUInt(out, value);
}

// counter32Val: uint32_t as uint32 (identity)
inline JsonReadError __counter32ValParse(const char *&p, const char *end, uint32_t &out) {
  return jsonReadUInt32(p, end, out);
}

inline void __counter32ValSerialize(xmstream &out, uint32_t value) {
  jsonWriteUInt(out, value);
}
