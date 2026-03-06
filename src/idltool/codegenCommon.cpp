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
    case EScalarType::String:  return "std::string";
    case EScalarType::Bool:    return "bool";
    case EScalarType::Int32:   return "int32_t";
    case EScalarType::Uint32:  return "uint32_t";
    case EScalarType::Int64:   return "int64_t";
    case EScalarType::Uint64:  return "uint64_t";
    case EScalarType::Double:  return "double";
    case EScalarType::Seconds: return "std::chrono::seconds";
    case EScalarType::Minutes: return "std::chrono::minutes";
    case EScalarType::Hours:   return "std::chrono::hours";
  }
  return "void";
}

std::string fieldCppName(const std::string &name, bool pascalCase)
{
  if (!pascalCase || name.empty()) return name;
  std::string result = name;
  result[0] = toupper(result[0]);
  return result;
}

std::string cppFieldType(const CFieldDef &f, const std::unordered_set<std::string> &enumNames, const std::string &structPrefix)
{
  std::string base;
  if (f.Type.IsMapped) {
    base = f.Type.MappedCppType;
  } else if (!f.Type.RefName.empty()) {
    if (enumNames.count(f.Type.RefName))
      base = "E" + f.Type.RefName;
    else
      base = structPrefix + f.Type.RefName;
  } else {
    base = cppScalarType(f.Type.Scalar);
  }

  switch (f.Kind) {
    case EFieldKind::Required:
    case EFieldKind::Optional:
      return base;
    case EFieldKind::OptionalObject:
    case EFieldKind::NullableObject:
      return std::format("std::optional<{}>", base);
    case EFieldKind::Array:
      return std::format("std::vector<{}>", base);
    case EFieldKind::OptionalArray:
    case EFieldKind::NullableArray:
      return std::format("std::optional<std::vector<{}>>", base);
  }
  return base;
}

static bool isChronoType(EScalarType t)
{
  return t == EScalarType::Seconds || t == EScalarType::Minutes || t == EScalarType::Hours;
}

std::string cppDefault(const CFieldDef &f, const std::unordered_set<std::string> &enumNames, bool pascalCase)
{
  switch (f.Default.Kind) {
    case EDefaultKind::None: return "";
    case EDefaultKind::String: {
      if (!f.Type.RefName.empty() && enumNames.count(f.Type.RefName))
        return std::format("E{}::{}", f.Type.RefName, fieldCppName(f.Default.StringVal, pascalCase));
      return std::format("\"{}\"", f.Default.StringVal);
    }
    case EDefaultKind::Int: {
      if (f.Type.IsScalar && isChronoType(f.Type.Scalar))
        return std::format("{}({})", cppScalarType(f.Type.Scalar), f.Default.IntVal);
      return std::to_string(f.Default.IntVal);
    }
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
  return !f.Type.RefName.empty() && !f.Type.IsMapped && !enumNames.count(f.Type.RefName);
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

void generateEnumDeclarations(std::string &out, const CIdlFile &file, bool pascalCase)
{
  for (auto &e : file.Enums) {
    if (e.IsImported) continue;
    std::string enumType = "E" + e.Name;

    out += std::format("enum class {} {{\n", enumType);
    for (auto &v : e.Values)
      out += std::format("  {},\n", fieldCppName(v, pascalCase));
    out += "};\n\n";

    // Count constant
    out += std::format("inline constexpr unsigned {}Count = {};\n", enumType, e.Values.size());

    // Function declarations
    out += std::format("bool parse{}(const char *str, size_t len, {} &out);\n", enumType, enumType);
    out += std::format("const char *{}ToString({} v);\n", enumType, enumType);
    out += std::format("const std::vector<const char*> &{}Names();\n\n", enumType);
  }
}

void generateEnumDefinitions(std::string &out, const CIdlFile &file, bool pascalCase)
{
  for (auto &e : file.Enums) {
    if (e.IsImported) continue;
    std::string enumType = "E" + e.Name;

    // String -> enum
    out += std::format("bool parse{}(const char *str, size_t len, {} &out) {{\n", enumType, enumType);

    auto ph = findPerfectHash(e.Values);
    if (ph && e.Values.size() > 4) {
      // Perfect hash dispatch
      out += std::format("  uint32_t h = {}u;\n", ph->Seed);
      out += std::format("  for (size_t i = 0; i < len; i++) h = h * {}u + (unsigned char)str[i];\n", ph->Mult);
      out += std::format("  switch (h % {}u) {{\n", ph->Mod);
      for (size_t i = 0; i < e.Values.size(); i++) {
        auto &v = e.Values[i];
        uint32_t h = computeHash(v.c_str(), v.size(), ph->Seed, ph->Mult, ph->Mod);
        out += std::format("    case {}: if (len == {} && memcmp(str, \"{}\", {}) == 0) {{ out = {}::{}; return true; }} break;\n",
                           h, v.size(), v, v.size(), enumType, fieldCppName(v, pascalCase));
      }
      out += "    default: break;\n";
      out += "  }\n";
    } else {
      // Linear search for small enums
      for (auto &v : e.Values)
        out += std::format("  if (len == {} && memcmp(str, \"{}\", {}) == 0) {{ out = {}::{}; return true; }}\n",
                           v.size(), v, v.size(), enumType, fieldCppName(v, pascalCase));
    }

    out += "  return false;\n}\n\n";

    // Enum -> string
    out += std::format("const char *{}ToString({} v) {{\n", enumType, enumType);
    out += "  switch (v) {\n";
    for (auto &v : e.Values)
      out += std::format("    case {}::{}: return \"{}\";\n", enumType, fieldCppName(v, pascalCase), v);
    out += "  }\n  return \"\";\n}\n\n";

    // Names
    out += std::format("const std::vector<const char*> &{}Names() {{\n", enumType);
    out += "  static const std::vector<const char*> names = {";
    for (size_t i = 0; i < e.Values.size(); i++) {
      if (i) out += ", ";
      out += std::format("\"{}\"", e.Values[i]);
    }
    out += "};\n  return names;\n}\n\n";
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
    code += std::format("{}out.write('\"'); out.write(E{}ToString({})); out.write('\"');\n", in, f.Type.RefName, valueName);
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
    case EScalarType::Seconds: case EScalarType::Minutes: case EScalarType::Hours:
      code += std::format("{}jsonWriteInt(out, {}.count());\n", in, valueName); break;
  }
}

bool isMappedField(const CFieldDef &f)
{
  return f.Type.IsMapped;
}

void emitSerializeArrayElem(std::string &code, const CFieldDef &f,
                            const std::string &valueName,
                            const std::unordered_set<std::string> &enumNames,
                            int ind)
{
  if (isStructRef(f, enumNames)) {
    code += std::format("{}{}.serialize(out);\n", indent(ind), valueName);
    return;
  }
  emitSerializeValue(code, f, valueName, enumNames, ind);
}

void generateSerializeField(std::string &code, const CFieldDef &f, const std::unordered_set<std::string> &enumNames, int ind, bool &first, bool pascalCase)
{
  std::string in = indent(ind);
  std::string cn = fieldCppName(f.Name, pascalCase);
  auto emitKey = [&](const std::string &jsonKey) {
    if (!first)
      code += std::format("{}out.write(',');\n", in);
    first = false;
    code += std::format("{}out.write(\"\\\"{}\\\":\");\n", in, jsonKey);
  };

  // Mapped field: write empty string in standard serialize
  if (isMappedField(f)) {
    emitKey(f.Name);
    code += std::format("{}jsonWriteString(out, \"\");\n", in);
    return;
  }

  switch (f.Kind) {
    case EFieldKind::Required:
    case EFieldKind::Optional: {
      emitKey(f.Name);
      if (isStructRef(f, enumNames)) {
        code += std::format("{}{}.serialize(out);\n", in, cn);
      } else {
        emitSerializeValue(code, f, cn, enumNames, ind);
      }
      break;
    }

    case EFieldKind::OptionalObject: {
      code += std::format("{}if ({}.has_value()) {{\n", in, cn);
      std::string in2 = indent(ind + 1);
      if (!first)
        code += std::format("{}out.write(',');\n", in2);
      code += std::format("{}out.write(\"\\\"{}\\\":\");\n", in2, f.Name);
      if (isStructRef(f, enumNames)) {
        code += std::format("{}{}->serialize(out);\n", in2, cn);
      } else {
        emitSerializeValue(code, f, std::format("*{}", cn), enumNames, ind + 1);
      }
      first = false;
      code += std::format("{}}}\n", in);
      break;
    }

    case EFieldKind::NullableObject: {
      emitKey(f.Name);
      code += std::format("{}if ({}.has_value()) {{\n", in, cn);
      std::string in2 = indent(ind + 1);
      if (isStructRef(f, enumNames)) {
        code += std::format("{}{}->serialize(out);\n", in2, cn);
      } else {
        emitSerializeValue(code, f, std::format("*{}", cn), enumNames, ind + 1);
      }
      code += std::format("{}}} else {{\n", in);
      code += std::format("{}out.write(\"null\");\n", indent(ind + 1));
      code += std::format("{}}}\n", in);
      break;
    }

    case EFieldKind::Array: {
      emitKey(f.Name);
      code += std::format("{}out.write('[');\n", in);
      code += std::format("{}for (size_t i_ = 0; i_ < {}.size(); i_++) {{\n", in, cn);
      code += std::format("{}  if (i_) out.write(',');\n", in);
      emitSerializeArrayElem(code, f, std::format("{}[i_]", cn), enumNames, ind + 1);
      code += std::format("{}}}\n", in);
      code += std::format("{}out.write(']');\n", in);
      break;
    }

    case EFieldKind::OptionalArray: {
      code += std::format("{}if ({}.has_value()) {{\n", in, cn);
      std::string in2 = indent(ind + 1);
      if (!first)
        code += std::format("{}out.write(',');\n", in2);
      code += std::format("{}out.write(\"\\\"{}\\\":\");\n", in2, f.Name);
      code += std::format("{}out.write('[');\n", in2);
      code += std::format("{}for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", in2, cn);
      code += std::format("{}  if (i_) out.write(',');\n", in2);
      emitSerializeArrayElem(code, f, std::format("(*{})[i_]", cn), enumNames, ind + 2);
      code += std::format("{}}}\n", in2);
      code += std::format("{}out.write(']');\n", in2);
      first = false;
      code += std::format("{}}}\n", in);
      break;
    }

    case EFieldKind::NullableArray: {
      emitKey(f.Name);
      code += std::format("{}if ({}.has_value()) {{\n", in, cn);
      std::string in2 = indent(ind + 1);
      code += std::format("{}out.write('[');\n", in2);
      code += std::format("{}for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", in2, cn);
      code += std::format("{}  if (i_) out.write(',');\n", in2);
      emitSerializeArrayElem(code, f, std::format("(*{})[i_]", cn), enumNames, ind + 2);
      code += std::format("{}}}\n", in2);
      code += std::format("{}out.write(']');\n", in2);
      code += std::format("{}}} else {{\n", in);
      code += std::format("{}out.write(\"null\");\n", indent(ind + 1));
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
    while (p < end) {
      if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; continue; }
      if (p + 1 < end && p[0] == '/') {
        if (p[1] == '/') { p += 2; while (p < end && *p != '\n') p++; continue; }
        if (p[1] == '*') { p += 2; while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++; if (p + 1 < end) p += 2; continue; }
      }
      break;
    }
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
)";

// --- JSON write helpers (static in generated source) ---

const char *jsonHelperCode = R"(
static char jsonHexDigit(uint8_t b) {
  return b < 10 ? '0' + b : 'a' + b - 10;
}

static void jsonWriteString(xmstream &out, std::string_view value) {
  out.write('"');
  for (char ch : value) {
    switch (ch) {
      case '"':  out.write("\\\""); break;
      case '\\': out.write("\\\\"); break;
      case '\b': out.write("\\b"); break;
      case '\f': out.write("\\f"); break;
      case '\n': out.write("\\n"); break;
      case '\r': out.write("\\r"); break;
      case '\t': out.write("\\t"); break;
      default:
        if (static_cast<uint8_t>(ch) < 0x20) {
          out.write("\\u00");
          out.write(jsonHexDigit(static_cast<uint8_t>(ch) >> 4));
          out.write(jsonHexDigit(static_cast<uint8_t>(ch) & 0xF));
        } else {
          out.write(ch);
        }
        break;
    }
  }
  out.write('"');
}

static void jsonWriteInt(xmstream &out, int64_t v) {
  char buf[24];
  int n = snprintf(buf, sizeof(buf), "%" PRId64, v);
  out.write(buf, n);
}

static void jsonWriteUInt(xmstream &out, uint64_t v) {
  char buf[24];
  int n = snprintf(buf, sizeof(buf), "%" PRIu64, v);
  out.write(buf, n);
}

static void jsonWriteDouble(xmstream &out, double v) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%.12g", v);
  out.write(buf, n);
}

static void jsonWriteBool(xmstream &out, bool v) {
  out.write(v ? "true" : "false");
}
)";
