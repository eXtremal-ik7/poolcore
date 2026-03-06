#include "codegenVerbose.h"
#include "codegenCommon.h"
#include <format>

// --- ParseError code for generated headers ---

const char *parseErrorCode = R"(
struct ParseError {
  int row = 0;
  int col = 0;
  std::string message;
};
)";

// --- VerboseJsonScanner code for generated source files ---

const char *verboseJsonScannerCode = R"(
struct VerboseJsonScanner {
  const char *p;
  const char *end;
  const char *begin;
  ParseError *error;

  void computePosition(int &row, int &col) const {
    row = 1;
    col = 1;
    for (const char *c = begin; c < p; c++) {
      if (*c == '\n') { row++; col = 1; }
      else col++;
    }
  }

  void setError(const std::string &msg) {
    if (!error || !error->message.empty()) return;
    computePosition(error->row, error->col);
    error->message = msg;
  }

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
    if (p >= end)
      setError(std::string("expected '") + c + "', got end of input");
    else
      setError(std::string("expected '") + c + "', got '" + *p + "'");
    return false;
  }

  bool readString(const char *&str, size_t &len) {
    skipWhitespace();
    if (p >= end) { setError("expected string, got end of input"); return false; }
    if (*p != '"') { setError(std::string("expected string, got '") + *p + "'"); return false; }
    p++;
    str = p;
    while (p < end && *p != '"') {
      if (*p == '\\') { p++; if (p < end) p++; else { setError("unterminated string"); return false; } }
      else p++;
    }
    if (p >= end) { setError("unterminated string"); return false; }
    len = p - str;
    p++;
    return true;
  }

  bool readStringHash(const char *&str, size_t &len, uint32_t &hash, uint32_t seed, uint32_t mult) {
    skipWhitespace();
    if (p >= end) { setError("expected string, got end of input"); return false; }
    if (*p != '"') { setError(std::string("expected string, got '") + *p + "'"); return false; }
    p++;
    str = p;
    uint32_t h = seed;
    while (p < end && *p != '"') {
      if (*p == '\\') {
        h = h * mult + (unsigned char)*p; p++;
        if (p < end) { h = h * mult + (unsigned char)*p; p++; }
        else { setError("unterminated string"); return false; }
      } else {
        h = h * mult + (unsigned char)*p; p++;
      }
    }
    if (p >= end) { setError("unterminated string"); return false; }
    len = p - str;
    hash = h;
    p++;
    return true;
  }

  bool readStringValue(std::string &out) {
    skipWhitespace();
    if (p >= end) { setError("expected string, got end of input"); return false; }
    if (*p != '"') { setError(std::string("expected string, got '") + *p + "'"); return false; }
    p++;
    out.clear();
    while (p < end && *p != '"') {
      if (*p == '\\') {
        p++;
        if (p >= end) { setError("unterminated string"); return false; }
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
            if (p + 5 <= end) {
              auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
              };
              int d0 = hexVal(p[1]), d1 = hexVal(p[2]), d2 = hexVal(p[3]), d3 = hexVal(p[4]);
              if (d0 >= 0 && d1 >= 0 && d2 >= 0 && d3 >= 0) {
                uint32_t cp = (d0 << 12) | (d1 << 8) | (d2 << 4) | d3;
                if (cp < 0x80) {
                  out += (char)cp;
                } else if (cp < 0x800) {
                  out += (char)(0xC0 | (cp >> 6));
                  out += (char)(0x80 | (cp & 0x3F));
                } else {
                  out += (char)(0xE0 | (cp >> 12));
                  out += (char)(0x80 | ((cp >> 6) & 0x3F));
                  out += (char)(0x80 | (cp & 0x3F));
                }
                p += 4;
              } else {
                out += '\\'; out += 'u';
              }
            } else {
              out += '\\'; out += 'u';
            }
            break;
          }
          default: out += *p; break;
        }
        p++;
      } else {
        out += *p++;
      }
    }
    if (p >= end) { setError("unterminated string"); return false; }
    p++;
    return true;
  }

  bool readInt64(int64_t &out) {
    skipWhitespace();
    if (p >= end) { setError("expected integer, got end of input"); return false; }
    char *ep;
    out = strtoll(p, &ep, 10);
    if (ep == p) { setError(std::string("expected integer, got '") + *p + "'"); return false; }
    p = ep;
    return true;
  }

  bool readUInt64(uint64_t &out) {
    skipWhitespace();
    if (p >= end) { setError("expected integer, got end of input"); return false; }
    if (*p == '-') { setError("expected unsigned integer, got negative value"); return false; }
    char *ep;
    out = strtoull(p, &ep, 10);
    if (ep == p) { setError(std::string("expected integer, got '") + *p + "'"); return false; }
    p = ep;
    return true;
  }

  bool readInt32(int32_t &out) {
    int64_t v;
    if (!readInt64(v)) return false;
    if (v < INT32_MIN || v > INT32_MAX) { setError("integer out of int32 range"); return false; }
    out = (int32_t)v;
    return true;
  }

  bool readUInt32(uint32_t &out) {
    uint64_t v;
    if (!readUInt64(v)) return false;
    if (v > UINT32_MAX) { setError("integer out of uint32 range"); return false; }
    out = (uint32_t)v;
    return true;
  }

  bool readDouble(double &out) {
    skipWhitespace();
    if (p >= end) { setError("expected number, got end of input"); return false; }
    char *ep;
    out = strtod(p, &ep);
    if (ep == p) { setError(std::string("expected number, got '") + *p + "'"); return false; }
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
    if (p >= end)
      setError("expected boolean, got end of input");
    else
      setError(std::string("expected boolean, got '") + *p + "'");
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
    if (p >= end) { setError("unexpected end of input"); return false; }
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
      case 't': return (end - p >= 4 && memcmp(p, "true", 4) == 0) ? (p += 4, true) : (setError("invalid literal"), false);
      case 'f': return (end - p >= 5 && memcmp(p, "false", 5) == 0) ? (p += 5, true) : (setError("invalid literal"), false);
      case 'n': return (end - p >= 4 && memcmp(p, "null", 4) == 0) ? (p += 4, true) : (setError("invalid literal"), false);
      default: {
        if (*p == '-') p++;
        if (p >= end || (*p < '0' || *p > '9')) { setError("expected value"); return false; }
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

// --- Verbose parse field generation ---

struct VerboseWireTypeInfo {
  const char *readMethod;
  const char *cppType;
};

static VerboseWireTypeInfo getVerboseWireTypeInfo(const std::string &wireType)
{
  if (wireType == "string") return {"readStringValue", "std::string"};
  if (wireType == "int64")  return {"readInt64", "int64_t"};
  if (wireType == "uint64") return {"readUInt64", "uint64_t"};
  if (wireType == "int32")  return {"readInt32", "int32_t"};
  if (wireType == "uint32") return {"readUInt32", "uint32_t"};
  if (wireType == "double") return {"readDouble", "double"};
  if (wireType == "bool")   return {"readBool", "bool"};
  return {"readStringValue", "std::string"};
}

static void generateVerboseParseScalar(std::string &out, const CFieldDef &f, const std::unordered_set<std::string> &enumNames, int foundBit, int ind, const std::string &fieldContext, bool pascalCase)
{
  std::string in = indent(ind);
  std::string cn = fieldCppName(f.Name, pascalCase);

  // Mapped field: parse wire value + resolve into destination value.
  if (isMappedField(f)) {
    auto wi = getVerboseWireTypeInfo(f.Type.MappedWireType);
    std::string resolveFunc = "__" + f.Type.RefName + "Resolve";
    out += std::format("{}{{ {} _tmp;\n", in, wi.cppType);
    out += std::format("{}  if (!s.{}(_tmp)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }}\n",
                       in, wi.readMethod, fieldContext);
    out += std::format("{}  if (!{}(_tmp, {})) {{ s.setError(\"field '{}': invalid mapped value\"); return false; }}\n",
                       in, resolveFunc, cn, fieldContext);
    if (foundBit >= 0)
      out += std::format("{}  found |= (uint64_t)1 << {};\n", in, foundBit);
    out += std::format("{}}}\n", in);
    return;
  }

  if (isEnum(f.Type.RefName, enumNames)) {
    out += std::format("{}{{ const char *eStr; size_t eLen;\n", in);
    out += std::format("{}  if (!s.readString(eStr, eLen)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }}\n",
                       in, fieldContext);
    out += std::format("{}  if (!parseE{}(eStr, eLen, {})) {{\n", in, f.Type.RefName, cn);
    out += std::format("{}    s.error->message = \"field '{}': invalid enum value '\" + std::string(eStr, eLen) + \"'\";\n",
                       in, fieldContext);
    out += std::format("{}    return false;\n", in);
    out += std::format("{}  }}\n", in);
    if (foundBit >= 0)
      out += std::format("{}  found |= (uint64_t)1 << {};\n", in, foundBit);
    out += std::format("{}}}\n", in);
    return;
  }

  // Chrono types: read as int64 then construct
  if (f.Type.IsScalar && (f.Type.Scalar == EScalarType::Seconds ||
      f.Type.Scalar == EScalarType::Minutes || f.Type.Scalar == EScalarType::Hours)) {
    std::string chronoType = cppScalarType(f.Type.Scalar);
    out += std::format("{}{{ int64_t _t;\n", in);
    out += std::format("{}  if (!s.readInt64(_t)) {{\n", in);
    out += std::format("{}    s.error->message = \"field '{}': \" + s.error->message;\n", in, fieldContext);
    out += std::format("{}    return false;\n", in);
    out += std::format("{}  }}\n", in);
    out += std::format("{}  {} = {}(_t);\n", in, cn, chronoType);
    if (foundBit >= 0)
      out += std::format("{}  found |= (uint64_t)1 << {};\n", in, foundBit);
    out += std::format("{}}}\n", in);
    return;
  }

  const char *readMethod = nullptr;
  switch (f.Type.Scalar) {
    case EScalarType::String: readMethod = "readStringValue"; break;
    case EScalarType::Bool:   readMethod = "readBool"; break;
    case EScalarType::Int32:  readMethod = "readInt32"; break;
    case EScalarType::Uint32: readMethod = "readUInt32"; break;
    case EScalarType::Int64:  readMethod = "readInt64"; break;
    case EScalarType::Uint64: readMethod = "readUInt64"; break;
    case EScalarType::Double: readMethod = "readDouble"; break;
    default: break;
  }

  out += std::format("{}if (!s.{}({})) {{\n", in, readMethod, cn);
  out += std::format("{}  s.error->message = \"field '{}': \" + s.error->message;\n", in, fieldContext);
  out += std::format("{}  return false;\n", in);
  out += std::format("{}}}\n", in);
  if (foundBit >= 0)
    out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
}

void generateVerboseParseField(std::string &out, const CFieldDef &f, const std::unordered_set<std::string> &enumNames, int foundBit, int ind, bool pascalCase)
{
  std::string in = indent(ind);
  std::string ctx = f.Name; // field context for error messages (original IDL name)
  std::string cn = fieldCppName(f.Name, pascalCase);

  switch (f.Kind) {
    case EFieldKind::Required:
    case EFieldKind::Optional: {
      if (isStructRef(f, enumNames)) {
        out += std::format("{}if (!{}.parseVerboseImpl(s)) return false;\n", in, cn);
        if (foundBit >= 0)
          out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      } else {
        generateVerboseParseScalar(out, f, enumNames, foundBit, ind, ctx, pascalCase);
      }
      break;
    }

    case EFieldKind::OptionalObject:
    case EFieldKind::NullableObject: {
      out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      if (isStructRef(f, enumNames)) {
        out += std::format("{}else if (!{}.emplace().parseVerboseImpl(s)) return false;\n", in, cn);
      } else {
        out += std::format("{}else {{\n", in);
        out += std::format("{}  {}.emplace();\n", in, cn);
        CFieldDef tmp = f;
        tmp.Name = std::format("(*{})", cn);
        tmp.Kind = EFieldKind::Required;
        generateVerboseParseScalar(out, tmp, enumNames, -1, ind + 1, ctx, false);
        out += std::format("{}}}\n", in);
      }
      break;
    }

    case EFieldKind::Array: {
      out += std::format("{}if (!s.expectChar('[')) {{ s.error->message = \"field '{}': expected array\"; return false; }}\n", in, ctx);
      out += std::format("{}s.skipWhitespace();\n", in);
      out += std::format("{}if (s.p < s.end && *s.p != ']') {{\n", in);
      out += std::format("{}  for (;;) {{\n", in);

      if (isMappedField(f)) {
        auto wi = getVerboseWireTypeInfo(f.Type.MappedWireType);
        std::string resolveFunc = "__" + f.Type.RefName + "Resolve";
        out += std::format("{}    {{ {} _tmp; {}.emplace_back();\n", in, wi.cppType, cn);
        out += std::format("{}      if (!s.{}(_tmp)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }}\n",
                           in, wi.readMethod, ctx);
        out += std::format("{}      if (!{}(_tmp, {}.back())) {{ s.setError(\"field '{}': invalid mapped value in array\"); return false; }}\n",
                           in, resolveFunc, cn, ctx);
        out += std::format("{}    }}\n", in);
      } else if (isStructRef(f, enumNames)) {
        out += std::format("{}    if (!{}.emplace_back().parseVerboseImpl(s)) return false;\n", in, cn);
      } else if (isEnum(f.Type.RefName, enumNames)) {
        out += std::format("{}    {{\n", in);
        out += std::format("{}      const char *eStr; size_t eLen;\n", in);
        out += std::format("{}      {}.emplace_back();\n", in, cn);
        out += std::format("{}      if (!s.readString(eStr, eLen)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }}\n",
                           in, ctx);
        out += std::format("{}      if (!parseE{}(eStr, eLen, {}.back())) {{\n", in, f.Type.RefName, cn);
        out += std::format("{}        s.error->message = \"field '{}': invalid enum value '\" + std::string(eStr, eLen) + \"' in array\";\n", in, ctx);
        out += std::format("{}        return false;\n", in);
        out += std::format("{}      }}\n", in);
        out += std::format("{}    }}\n", in);
      } else if (f.Type.IsScalar && (f.Type.Scalar == EScalarType::Seconds ||
                 f.Type.Scalar == EScalarType::Minutes || f.Type.Scalar == EScalarType::Hours)) {
        std::string chronoType = cppScalarType(f.Type.Scalar);
        out += std::format("{}    {{ int64_t _t; if (!s.readInt64(_t)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }} "
                           "{}.push_back({}(_t)); }}\n", in, ctx, cn, chronoType);
      } else {
        const char *cppType = nullptr;
        const char *readMethod = nullptr;
        switch (f.Type.Scalar) {
          case EScalarType::String: cppType = "std::string"; readMethod = "readStringValue"; break;
          case EScalarType::Bool:   cppType = "bool"; readMethod = "readBool"; break;
          case EScalarType::Int32:  cppType = "int32_t"; readMethod = "readInt32"; break;
          case EScalarType::Uint32: cppType = "uint32_t"; readMethod = "readUInt32"; break;
          case EScalarType::Int64:  cppType = "int64_t"; readMethod = "readInt64"; break;
          case EScalarType::Uint64: cppType = "uint64_t"; readMethod = "readUInt64"; break;
          case EScalarType::Double: cppType = "double"; readMethod = "readDouble"; break;
          default: cppType = "int64_t"; readMethod = "readInt64"; break;
        }
        if (f.Type.Scalar == EScalarType::String) {
          out += std::format("{}    {{ std::string tmp; if (!s.{}(tmp)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }} "
                             "{}.push_back(std::move(tmp)); }}\n", in, readMethod, ctx, cn);
        } else {
          out += std::format("{}    {{ {} tmp; if (!s.{}(tmp)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }} "
                             "{}.push_back(tmp); }}\n", in, cppType, readMethod, ctx, cn);
        }
      }

      out += std::format("{}    s.skipWhitespace();\n", in);
      out += std::format("{}    if (s.p < s.end && *s.p == ',') {{ s.p++; continue; }}\n", in);
      out += std::format("{}    break;\n", in);
      out += std::format("{}  }}\n", in);
      out += std::format("{}}}\n", in);
      out += std::format("{}if (!s.expectChar(']')) return false;\n", in);
      if (foundBit >= 0)
        out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::OptionalArray:
    case EFieldKind::NullableArray: {
      out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      out += std::format("{}else {{\n", in);
      out += std::format("{}  {}.emplace();\n", in, cn);
      CFieldDef tmp = f;
      tmp.Name = std::format("(*{})", cn);
      tmp.Kind = EFieldKind::Array;
      generateVerboseParseField(out, tmp, enumNames, -1, ind + 1, false);
      out += std::format("{}}}\n", in);
      break;
    }
  }
}
