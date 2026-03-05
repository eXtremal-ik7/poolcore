#include "codegenDiag.h"
#include "codegenCommon.h"
#include <format>

// --- DiagJsonScanner code for generated headers ---

const char *diagJsonScannerCode = R"(
struct ParseError {
  int row = 0;
  int col = 0;
  std::string message;
};

struct DiagJsonScanner {
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
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
      p++;
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
          case 'u': out += '\\'; out += 'u'; break;
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
    char *ep;
    out = strtoull(p, &ep, 10);
    if (ep == p) { setError(std::string("expected integer, got '") + *p + "'"); return false; }
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

// --- Diagnostic parse field generation ---

// Helper: name of the expected type for error messages
static const char *scalarExpectedType(EScalarType t)
{
  switch (t) {
    case EScalarType::String: return "string";
    case EScalarType::Bool:   return "boolean";
    case EScalarType::Int32:  return "integer";
    case EScalarType::Uint32: return "integer";
    case EScalarType::Int64:  return "integer";
    case EScalarType::Uint64: return "integer";
    case EScalarType::Double: return "number";
  }
  return "value";
}

static void generateDiagParseScalar(std::string &out, const CFieldDef &f,
                                     const std::unordered_set<std::string> &enumNames,
                                     int foundBit, int ind,
                                     const std::string &fieldContext)
{
  std::string in = indent(ind);

  if (isEnum(f.Type.RefName, enumNames)) {
    out += std::format("{}{{ const char *eStr; size_t eLen;\n", in);
    out += std::format("{}  if (!s.readString(eStr, eLen)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }}\n",
                       in, fieldContext);
    out += std::format("{}  if (!parseE{}(eStr, eLen, {})) {{\n", in, f.Type.RefName, f.Name);
    out += std::format("{}    s.error->message = \"field '{}': invalid enum value '\" + std::string(eStr, eLen) + \"'\";\n",
                       in, fieldContext);
    out += std::format("{}    return false;\n", in);
    out += std::format("{}  }}\n", in);
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
  }

  out += std::format("{}if (!s.{}({})) {{\n", in, readMethod, f.Name);
  out += std::format("{}  s.error->message = \"field '{}': \" + s.error->message;\n", in, fieldContext);
  out += std::format("{}  return false;\n", in);
  out += std::format("{}}}\n", in);
  if (foundBit >= 0)
    out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
}

void generateDiagParseField(std::string &out, const CFieldDef &f,
                            const std::unordered_set<std::string> &enumNames,
                            int foundBit, int ind)
{
  std::string in = indent(ind);
  std::string ctx = f.Name; // field context for error messages

  switch (f.Kind) {
    case EFieldKind::Required:
    case EFieldKind::Optional: {
      if (isStructRef(f, enumNames)) {
        out += std::format("{}if (!{}.parseDiagImpl(s)) return false;\n", in, f.Name);
        if (foundBit >= 0)
          out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      } else {
        generateDiagParseScalar(out, f, enumNames, foundBit, ind, ctx);
      }
      break;
    }

    case EFieldKind::OptionalObject: {
      out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      if (isStructRef(f, enumNames)) {
        out += std::format("{}else if (!{}.emplace().parseDiagImpl(s)) return false;\n", in, f.Name);
      } else {
        out += std::format("{}else {{\n", in);
        out += std::format("{}  {}.emplace();\n", in, f.Name);
        CFieldDef tmp = f;
        tmp.Name = std::format("(*{})", f.Name);
        tmp.Kind = EFieldKind::Required;
        generateDiagParseScalar(out, tmp, enumNames, -1, ind + 1, ctx);
        out += std::format("{}}}\n", in);
      }
      break;
    }

    case EFieldKind::Array: {
      out += std::format("{}if (!s.expectChar('[')) {{ s.error->message = \"field '{}': expected array\"; return false; }}\n", in, ctx);
      out += std::format("{}s.skipWhitespace();\n", in);
      out += std::format("{}if (s.p < s.end && *s.p != ']') {{\n", in);
      out += std::format("{}  for (;;) {{\n", in);

      if (isStructRef(f, enumNames)) {
        out += std::format("{}    if (!{}.emplace_back().parseDiagImpl(s)) return false;\n", in, f.Name);
      } else if (isEnum(f.Type.RefName, enumNames)) {
        out += std::format("{}    {{\n", in);
        out += std::format("{}      const char *eStr; size_t eLen;\n", in);
        out += std::format("{}      {}.emplace_back();\n", in, f.Name);
        out += std::format("{}      if (!s.readString(eStr, eLen)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }}\n",
                           in, ctx);
        out += std::format("{}      if (!parseE{}(eStr, eLen, {}.back())) {{\n", in, f.Type.RefName, f.Name);
        out += std::format("{}        s.error->message = \"field '{}': invalid enum value '\" + std::string(eStr, eLen) + \"' in array\";\n", in, ctx);
        out += std::format("{}        return false;\n", in);
        out += std::format("{}      }}\n", in);
        out += std::format("{}    }}\n", in);
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
        }
        if (f.Type.Scalar == EScalarType::String) {
          out += std::format("{}    {{ std::string tmp; if (!s.{}(tmp)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }} "
                             "{}.push_back(std::move(tmp)); }}\n", in, readMethod, ctx, f.Name);
        } else {
          out += std::format("{}    {{ {} tmp; if (!s.{}(tmp)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }} "
                             "{}.push_back(tmp); }}\n", in, cppType, readMethod, ctx, f.Name);
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

    case EFieldKind::OptionalArray: {
      out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      out += std::format("{}else {{\n", in);
      out += std::format("{}  {}.emplace();\n", in, f.Name);
      CFieldDef tmp = f;
      tmp.Name = std::format("(*{})", f.Name);
      tmp.Kind = EFieldKind::Array;
      generateDiagParseField(out, tmp, enumNames, -1, ind + 1);
      out += std::format("{}}}\n", in);
      break;
    }
  }
}
