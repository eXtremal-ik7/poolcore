#include "codegenCommon.h"
#include <cstdio>
#include <cstdint>
#include <format>
#include <algorithm>
#include <unordered_map>

// --- Helpers ---

std::string indent(int level)
{
  return std::string(level * 2, ' ');
}

std::string cppScalarType(EScalarType t)
{
  switch (t) {
    case EScalarType::String: return "std::string";
    case EScalarType::Bool:   return "bool";
    case EScalarType::Int32:  return "int32_t";
    case EScalarType::Uint32: return "uint32_t";
    case EScalarType::Int64:  return "int64_t";
    case EScalarType::Uint64: return "uint64_t";
    case EScalarType::Double: return "double";
  }
  return "void";
}

std::string cppFieldType(const CFieldDef &f, const std::unordered_set<std::string> &enumNames)
{
  std::string base;
  if (!f.Type.RefName.empty()) {
    if (enumNames.count(f.Type.RefName))
      base = "E" + f.Type.RefName;
    else
      base = f.Type.RefName;
  } else {
    base = cppScalarType(f.Type.Scalar);
  }

  switch (f.Kind) {
    case EFieldKind::Required:
    case EFieldKind::Optional:
      return base;
    case EFieldKind::OptionalObject:
      return std::format("std::optional<{}>", base);
    case EFieldKind::Array:
      return std::format("std::vector<{}>", base);
    case EFieldKind::OptionalArray:
      return std::format("std::optional<std::vector<{}>>", base);
  }
  return base;
}

std::string cppDefault(const CFieldDef &f, const std::unordered_set<std::string> &enumNames)
{
  switch (f.Default.Kind) {
    case EDefaultKind::None: return "";
    case EDefaultKind::String: {
      if (!f.Type.RefName.empty() && enumNames.count(f.Type.RefName))
        return std::format("E{}::{}", f.Type.RefName, f.Default.StringVal);
      return std::format("\"{}\"", f.Default.StringVal);
    }
    case EDefaultKind::Int: return std::to_string(f.Default.IntVal);
    case EDefaultKind::Float: {
      auto s = std::format("{}", f.Default.FloatVal);
      if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
        s += ".0";
      return s;
    }
    case EDefaultKind::Bool: return f.Default.BoolVal ? "true" : "false";
    case EDefaultKind::RuntimeNow: return "time(nullptr)";
    case EDefaultKind::RuntimeNowOffset:
      return std::format("time(nullptr) - {}", f.Default.IntVal);
  }
  return "";
}

bool isEnum(const std::string &refName, const std::unordered_set<std::string> &enumNames)
{
  return !refName.empty() && enumNames.count(refName);
}

bool isStructRef(const CFieldDef &f, const std::unordered_set<std::string> &enumNames)
{
  return !f.Type.RefName.empty() && !enumNames.count(f.Type.RefName);
}

// --- Perfect hash ---

uint32_t computeHash(const char *key, size_t len, uint32_t seed, uint32_t mult, uint32_t mod)
{
  uint32_t h = seed;
  for (size_t i = 0; i < len; i++)
    h = h * mult + (unsigned char)key[i];
  return h % mod;
}

std::optional<CPerfectHash> findPerfectHash(const std::vector<std::string> &keys)
{
  if (keys.empty()) return CPerfectHash{0, 1, 1};
  if (keys.size() == 1) return CPerfectHash{0, 1, 1};

  static const uint32_t multipliers[] = {
    31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 97,
    101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151, 157, 163,
    167, 173, 179, 181, 191, 193, 197, 199, 211, 223, 227, 229, 233
  };

  size_t n = keys.size();
  for (uint32_t mod = (uint32_t)n; mod <= n * 4; mod++) {
    for (uint32_t seed = 0; seed < 256; seed++) {
      for (auto mult : multipliers) {
        std::unordered_set<uint32_t> hashes;
        bool ok = true;
        for (auto &k : keys) {
          uint32_t h = computeHash(k.c_str(), k.size(), seed, mult, mod);
          if (!hashes.insert(h).second) { ok = false; break; }
        }
        if (ok) return CPerfectHash{seed, mult, mod};
      }
    }
  }
  return std::nullopt;
}

// --- Enum generation ---

void generateEnumParsers(std::string &out, const CIdlFile &file)
{
  for (auto &e : file.Enums) {
    std::string enumType = "E" + e.Name;

    out += std::format("enum class {} {{\n", enumType);
    for (auto &v : e.Values)
      out += std::format("  {},\n", v);
    out += "};\n\n";

    // String -> enum
    out += std::format("static inline bool parse{}(const char *str, size_t len, {} &out) {{\n", enumType, enumType);
    for (auto &v : e.Values)
      out += std::format("  if (len == {} && memcmp(str, \"{}\", {}) == 0) {{ out = {}::{}; return true; }}\n",
                         v.size(), v, v.size(), enumType, v);
    out += "  return false;\n}\n\n";

    // Enum -> string
    out += std::format("static inline const char *{}ToString({} v) {{\n", enumType, enumType);
    out += "  switch (v) {\n";
    for (auto &v : e.Values)
      out += std::format("    case {}::{}: return \"{}\";\n", enumType, v, v);
    out += "  }\n  return \"\";\n}\n\n";
  }
}

// --- Serialize generation ---

void emitSerializeValue(std::string &code, const CFieldDef &f,
                        const std::string &valueName,
                        const std::unordered_set<std::string> &enumNames,
                        int ind)
{
  std::string in = indent(ind);
  if (isEnum(f.Type.RefName, enumNames)) {
    code += std::format("{}out += '\"'; out += E{}ToString({}); out += '\"';\n", in, f.Type.RefName, valueName);
    return;
  }
  switch (f.Type.Scalar) {
    case EScalarType::String:
      code += std::format("{}jsonWriteString(out, {});\n", in, valueName); break;
    case EScalarType::Bool:
      code += std::format("{}jsonWriteBool(out, {});\n", in, valueName); break;
    case EScalarType::Int32: case EScalarType::Int64:
      code += std::format("{}jsonWriteInt(out, {});\n", in, valueName); break;
    case EScalarType::Uint32: case EScalarType::Uint64:
      code += std::format("{}jsonWriteUInt(out, {});\n", in, valueName); break;
    case EScalarType::Double:
      code += std::format("{}jsonWriteDouble(out, {});\n", in, valueName); break;
  }
}

void emitSerializeArrayElem(std::string &code, const CFieldDef &f,
                            const std::string &valueName,
                            const std::unordered_set<std::string> &enumNames,
                            int ind)
{
  if (isStructRef(f, enumNames)) {
    code += std::format("{}{}.serializeImpl(out);\n", indent(ind), valueName);
    return;
  }
  emitSerializeValue(code, f, valueName, enumNames, ind);
}

void generateSerializeField(std::string &code, const CFieldDef &f,
                            const std::unordered_set<std::string> &enumNames,
                            int ind, bool &first)
{
  std::string in = indent(ind);
  auto emitKey = [&](const std::string &name) {
    if (!first)
      code += std::format("{}out += ',';\n", in);
    first = false;
    code += std::format("{}out += \"\\\"{}\\\":\";\n", in, name);
  };

  switch (f.Kind) {
    case EFieldKind::Required:
    case EFieldKind::Optional: {
      emitKey(f.Name);
      if (isStructRef(f, enumNames)) {
        code += std::format("{}{}.serializeImpl(out);\n", in, f.Name);
      } else {
        emitSerializeValue(code, f, f.Name, enumNames, ind);
      }
      break;
    }

    case EFieldKind::OptionalObject: {
      code += std::format("{}if ({}.has_value()) {{\n", in, f.Name);
      std::string in2 = indent(ind + 1);
      if (!first)
        code += std::format("{}out += ',';\n", in2);
      code += std::format("{}out += \"\\\"{}\\\":\";\n", in2, f.Name);
      if (isStructRef(f, enumNames)) {
        code += std::format("{}{}->serializeImpl(out);\n", in2, f.Name);
      } else {
        emitSerializeValue(code, f, std::format("*{}", f.Name), enumNames, ind + 1);
      }
      first = false;
      code += std::format("{}}}\n", in);
      break;
    }

    case EFieldKind::Array: {
      emitKey(f.Name);
      code += std::format("{}out += '[';\n", in);
      code += std::format("{}for (size_t i_ = 0; i_ < {}.size(); i_++) {{\n", in, f.Name);
      code += std::format("{}  if (i_) out += ',';\n", in);
      emitSerializeArrayElem(code, f, std::format("{}[i_]", f.Name), enumNames, ind + 1);
      code += std::format("{}}}\n", in);
      code += std::format("{}out += ']';\n", in);
      break;
    }

    case EFieldKind::OptionalArray: {
      code += std::format("{}if ({}.has_value()) {{\n", in, f.Name);
      std::string in2 = indent(ind + 1);
      if (!first)
        code += std::format("{}out += ',';\n", in2);
      code += std::format("{}out += \"\\\"{}\\\":[\";\n", in2, f.Name);
      code += std::format("{}for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", in2, f.Name);
      code += std::format("{}  if (i_) out += ',';\n", in2);
      emitSerializeArrayElem(code, f, std::format("(*{})[i_]", f.Name), enumNames, ind + 2);
      code += std::format("{}}}\n", in2);
      code += std::format("{}out += ']';\n", in2);
      first = false;
      code += std::format("{}}}\n", in);
      break;
    }
  }
}

// --- Scanner code ---

const char *jsonScannerCode = R"(
struct JsonScanner {
  const char *p;
  const char *end;

  void skipWhitespace() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
      p++;
  }

  bool expectChar(char c) {
    skipWhitespace();
    if (p < end && *p == c) { p++; return true; }
    return false;
  }

  bool readString(const char *&str, size_t &len) {
    skipWhitespace();
    if (p >= end || *p != '"') return false;
    p++;
    str = p;
    while (p < end && *p != '"') {
      if (*p == '\\') { p++; if (p < end) p++; else return false; }
      else p++;
    }
    if (p >= end) return false;
    len = p - str;
    p++;
    return true;
  }

  bool readStringHash(const char *&str, size_t &len, uint32_t &hash, uint32_t seed, uint32_t mult) {
    skipWhitespace();
    if (p >= end || *p != '"') return false;
    p++;
    str = p;
    uint32_t h = seed;
    while (p < end && *p != '"') {
      if (*p == '\\') {
        h = h * mult + (unsigned char)*p; p++;
        if (p < end) { h = h * mult + (unsigned char)*p; p++; } else return false;
      } else {
        h = h * mult + (unsigned char)*p; p++;
      }
    }
    if (p >= end) return false;
    len = p - str;
    hash = h;
    p++;
    return true;
  }

  bool readStringValue(std::string &out) {
    skipWhitespace();
    if (p >= end || *p != '"') return false;
    p++;
    out.clear();
    while (p < end && *p != '"') {
      if (*p == '\\') {
        p++;
        if (p >= end) return false;
        switch (*p) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            out += '\\'; out += 'u';
            break;
          }
          default: out += *p; break;
        }
        p++;
      } else {
        out += *p++;
      }
    }
    if (p >= end) return false;
    p++;
    return true;
  }

  bool readInt64(int64_t &out) {
    skipWhitespace();
    if (p >= end) return false;
    char *ep;
    out = strtoll(p, &ep, 10);
    if (ep == p) return false;
    p = ep;
    return true;
  }

  bool readUInt64(uint64_t &out) {
    skipWhitespace();
    if (p >= end) return false;
    char *ep;
    out = strtoull(p, &ep, 10);
    if (ep == p) return false;
    p = ep;
    return true;
  }

  bool readInt32(int32_t &out) {
    int64_t v;
    if (!readInt64(v)) return false;
    out = (int32_t)v;
    return true;
  }

  bool readUInt32(uint32_t &out) {
    uint64_t v;
    if (!readUInt64(v)) return false;
    out = (uint32_t)v;
    return true;
  }

  bool readDouble(double &out) {
    skipWhitespace();
    if (p >= end) return false;
    char *ep;
    out = strtod(p, &ep);
    if (ep == p) return false;
    p = ep;
    return true;
  }

  bool readBool(bool &out) {
    skipWhitespace();
    if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
      p += 4; out = true; return true;
    }
    if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
      p += 5; out = false; return true;
    }
    return false;
  }

  bool readNull() {
    skipWhitespace();
    if (end - p >= 4 && memcmp(p, "null", 4) == 0) {
      p += 4; return true;
    }
    return false;
  }

  bool skipValue() {
    skipWhitespace();
    if (p >= end) return false;
    switch (*p) {
      case '"': { const char *s; size_t l; return readString(s, l); }
      case '{': {
        p++;
        skipWhitespace();
        if (p < end && *p == '}') { p++; return true; }
        for (;;) {
          const char *s; size_t l;
          if (!readString(s, l)) return false;
          if (!expectChar(':')) return false;
          if (!skipValue()) return false;
          skipWhitespace();
          if (p < end && *p == ',') { p++; continue; }
          break;
        }
        return expectChar('}');
      }
      case '[': {
        p++;
        skipWhitespace();
        if (p < end && *p == ']') { p++; return true; }
        for (;;) {
          if (!skipValue()) return false;
          skipWhitespace();
          if (p < end && *p == ',') { p++; continue; }
          break;
        }
        return expectChar(']');
      }
      case 't': return (end - p >= 4 && memcmp(p, "true", 4) == 0) ? (p += 4, true) : false;
      case 'f': return (end - p >= 5 && memcmp(p, "false", 5) == 0) ? (p += 5, true) : false;
      case 'n': return (end - p >= 4 && memcmp(p, "null", 4) == 0) ? (p += 4, true) : false;
      default: {
        if (*p == '-') p++;
        if (p >= end || (*p < '0' || *p > '9')) return false;
        while (p < end && *p >= '0' && *p <= '9') p++;
        if (p < end && *p == '.') { p++; while (p < end && *p >= '0' && *p <= '9') p++; }
        if (p < end && (*p == 'e' || *p == 'E')) {
          p++;
          if (p < end && (*p == '+' || *p == '-')) p++;
          while (p < end && *p >= '0' && *p <= '9') p++;
        }
        return true;
      }
    }
  }
};

static inline char jsonHexDigit(uint8_t b) {
  return b < 10 ? '0' + b : 'a' + b - 10;
}

static inline void jsonWriteString(std::string &out, const std::string &value) {
  out += '"';
  for (char ch : value) {
    switch (ch) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<uint8_t>(ch) < 0x20) {
          out += "\\u00";
          out += jsonHexDigit(static_cast<uint8_t>(ch) >> 4);
          out += jsonHexDigit(static_cast<uint8_t>(ch) & 0xF);
        } else {
          out += ch;
        }
        break;
    }
  }
  out += '"';
}

static inline void jsonWriteInt(std::string &out, int64_t v) {
  out += std::to_string(v);
}

static inline void jsonWriteUInt(std::string &out, uint64_t v) {
  out += std::to_string(v);
}

static inline void jsonWriteDouble(std::string &out, double v) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.12g", v);
  out += buf;
}

static inline void jsonWriteBool(std::string &out, bool v) {
  out += v ? "true" : "false";
}
)";
