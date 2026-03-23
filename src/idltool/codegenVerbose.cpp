#include "codegenVerbose.h"
#include "codegenCommon.h"
#include <format>

// --- Verbose parse field generation ---

struct VerboseWireTypeInfo {
  const char *readMethod;
  const char *cppType;
  const char *typeName;
};

static VerboseWireTypeInfo getVerboseWireTypeInfo(const std::string &wireType)
{
  if (wireType == "string") return {"readStringValue", "std::string", "string"};
  if (wireType == "int64")  return {"readInt64", "int64_t", "integer"};
  if (wireType == "uint64") return {"readUInt64", "uint64_t", "unsigned integer"};
  if (wireType == "int32")  return {"readInt32", "int32_t", "integer"};
  if (wireType == "uint32") return {"readUInt32", "uint32_t", "unsigned integer"};
  if (wireType == "double") return {"readDouble", "double", "number"};
  if (wireType == "bool")   return {"readBool", "bool", "boolean"};
  return {"readStringValue", "std::string", "string"};
}

static void generateVerboseParseMappedValue(std::string &out,
                                            const std::string &mappedTypeName,
                                            const std::string &mappedWireType,
                                            const std::string &destExpr,
                                            const std::string &fieldContext,
                                            int ind,
                                            const std::string &failureCode)
{
  auto wi = getVerboseWireTypeInfo(mappedWireType);
  std::string in = indent(ind);
  std::string resolveName = "__" + mappedTypeName + "Resolve";
  out += std::format("{}{{ {} _tmp;\n", in, wi.cppType);
  out += std::format("{}  if (auto _e = s.{}(_tmp); _e != JsonReadError::Ok) {{ s.formatReadError(_e, \"{}\", \"{}\"); {}; }}\n",
                     in, wi.readMethod, fieldContext, wi.typeName, failureCode);
  out += std::format("{}  if (!{}(_tmp, {})) {{ s.setError(\"field '{}': invalid mapped value\"); {}; }}\n",
                     in, resolveName, destExpr, fieldContext, failureCode);
  out += std::format("{}}}\n", in);
}

static void generateVerboseParseScalar(std::string &out, const CFieldDef &f, const std::unordered_set<std::string> &enumNames, int foundBit, int ind, const std::string &fieldContext, bool pascalCase)
{
  std::string in = indent(ind);
  std::string cn = fieldCppName(f.Name, pascalCase);

  // Usertype field: direct parse
  if (f.Type.IsUserType) {
    std::string readName = "__" + f.Type.RefName + "Parse";
    out += std::format("{}if ({}(s.p, s.end, {}) != JsonReadError::Ok) {{ s.setError(\"field '{}': invalid {} value\"); return false; }}\n",
                       in, readName, cn, fieldContext, f.Type.RefName);
    if (foundBit >= 0)
      out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
    return;
  }

  // Derived field: parse wire value + resolve into destination value
  if (isDerivedField(f)) {
    auto wi = getVerboseWireTypeInfo(f.Type.MappedWireType);
    std::string resolveName = "__" + f.Type.RefName + "Resolve";
    out += std::format("{}{{ {} _tmp;\n", in, wi.cppType);
    out += std::format("{}  if (auto _e = s.{}(_tmp); _e != JsonReadError::Ok) {{ s.formatReadError(_e, \"{}\", \"{}\"); return false; }}\n",
                       in, wi.readMethod, fieldContext, wi.typeName);
    out += std::format("{}  if (!{}(_tmp, {})) {{ s.setError(\"field '{}': invalid derived value\"); return false; }}\n",
                       in, resolveName, cn, fieldContext);
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
    out += std::format("{}  if (auto _e = s.readInt64(_t); _e != JsonReadError::Ok) {{\n", in);
    out += std::format("{}    s.formatReadError(_e, \"{}\", \"integer\");\n", in, fieldContext);
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

  out += std::format("{}if (auto _e = s.{}({}); _e != JsonReadError::Ok) {{\n", in, readMethod, cn);
  out += std::format("{}  s.formatReadError(_e, \"{}\", \"{}\");\n", in, fieldContext, scalarTypeName(f.Type.Scalar));
  out += std::format("{}  return false;\n", in);
  out += std::format("{}}}\n", in);
  if (foundBit >= 0)
    out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
}

// Forward declarations
static void generateVerboseVariantParse(std::string &out, const CFieldDef &f,
    const std::string &varName, const std::string &ctx,
    const std::unordered_set<std::string> &enumNames, int ind);

static void generateVerboseParsePlainLeafElement(std::string &out, const CFieldDef &f,
    const std::unordered_set<std::string> &enumNames,
    const std::string &containerExpr, const std::string &ctx, int ind)
{
  std::string in = indent(ind);
  if (f.Type.IsUserType) {
    std::string readName = "__" + f.Type.RefName + "Parse";
    out += std::format("{}if ({}(s.p, s.end, {}) != JsonReadError::Ok) {{ s.setError(\"field '{}': invalid {} value\"); return false; }}\n",
                       in, readName, containerExpr, ctx, f.Type.RefName);
  } else if (isDerivedField(f)) {
    generateVerboseParseMappedValue(out, f.Type.RefName, f.Type.MappedWireType, containerExpr, ctx, ind, "return false");
  } else if (isStructRef(f, enumNames)) {
    out += std::format("{}if (!{}.parseVerboseImpl(s)) return false;\n", in, containerExpr);
  } else if (isEnum(f.Type.RefName, enumNames)) {
    out += std::format("{}{{ const char *eStr; size_t eLen;\n", in);
    out += std::format("{}  if (!s.readString(eStr, eLen)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }}\n", in, ctx);
    out += std::format("{}  if (!parseE{}(eStr, eLen, {})) {{ s.setError(\"field '{}': invalid enum value\"); return false; }} }}\n",
                       in, f.Type.RefName, containerExpr, ctx);
  } else if (f.Type.IsScalar && (f.Type.Scalar == EScalarType::Seconds ||
             f.Type.Scalar == EScalarType::Minutes || f.Type.Scalar == EScalarType::Hours)) {
    std::string chronoType = cppScalarType(f.Type.Scalar);
    out += std::format("{}{{ int64_t _t; if (auto _e = s.readInt64(_t); _e != JsonReadError::Ok) {{ s.formatReadError(_e, \"{}\", \"integer\"); return false; }} {} = {}(_t); }}\n",
                       in, ctx, containerExpr, chronoType);
  } else {
    const char *readMethod = "readInt64";
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
    out += std::format("{}if (auto _e = s.{}({}); _e != JsonReadError::Ok) {{ s.formatReadError(_e, \"{}\", \"{}\"); return false; }}\n",
                       in, readMethod, containerExpr, ctx, scalarTypeName(f.Type.Scalar));
  }
}

// Parse a map array element (verbose)
static void generateVerboseParseMapElement(std::string &out, const CFieldDef &f,
    const std::unordered_set<std::string> &enumNames,
    const std::string &containerExpr, const std::string &ctx, int ind)
{
  std::string in = indent(ind);
  out += std::format("{}if (!s.expectChar('{{')) {{ s.setError(\"field '{}': expected '{{'\"); return false; }}\n", in, ctx);
  out += std::format("{}s.skipWhitespace();\n", in);
  out += std::format("{}if (s.p < s.end && *s.p != '}}') {{\n", in);
  out += std::format("{}  for (;;) {{\n", in);
  out += std::format("{}    const char *_mapKey; size_t _mapKeyLen;\n", in);
  out += std::format("{}    if (!s.readString(_mapKey, _mapKeyLen)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }}\n", in, ctx);
  out += std::format("{}    if (!s.expectChar(':')) {{ s.setError(\"field '{}': expected ':'\"); return false; }}\n", in, ctx);
  out += std::format("{}    s.skipWhitespace();\n", in);
  out += std::format("{}    auto &_mapVal = {}[std::string(_mapKey, _mapKeyLen)];\n", in, containerExpr);

  if (isStructRef(f, enumNames)) {
    out += std::format("{}    if (!_mapVal.parseVerboseImpl(s)) return false;\n", in);
  } else if (isEnum(f.Type.RefName, enumNames)) {
    out += std::format("{}    {{ const char *eStr; size_t eLen;\n", in);
    out += std::format("{}      if (!s.readString(eStr, eLen)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }}\n", in, ctx);
    out += std::format("{}      if (!parseE{}(eStr, eLen, _mapVal)) {{ s.setError(\"field '{}': invalid enum value\"); return false; }} }}\n",
                       in, f.Type.RefName, ctx);
  } else {
    const char *readMethod = "readInt64";
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
    out += std::format("{}    if (auto _e = s.{}(_mapVal); _e != JsonReadError::Ok) {{ s.formatReadError(_e, \"{}\", \"{}\"); return false; }}\n",
                       in, readMethod, ctx, scalarTypeName(f.Type.Scalar));
  }

  out += std::format("{}    s.skipWhitespace();\n", in);
  out += std::format("{}    if (s.p < s.end && *s.p == ',') {{ s.p++; s.skipWhitespace(); continue; }}\n", in);
  out += std::format("{}    break;\n", in);
  out += std::format("{}  }}\n", in);
  out += std::format("{}}}\n", in);
  out += std::format("{}if (!s.expectCharNoWs('}}')) return false;\n", in);
}

static void generateVerboseNestedArrayParse(std::string &out, const CFieldDef &f,
    const std::unordered_set<std::string> &enumNames,
    const std::vector<CArrayDim> &dims, int dimIndex,
    const std::string &containerExpr, const std::string &ctx, int ind)
{
  std::string in = indent(ind);

  if (dimIndex >= (int)dims.size()) {
    // Leaf element — dispatch by array element kind
    switch (f.Type.ArrayElementKind) {
      case EArrayElementKind::Optional: {
        bool allowNull = (f.Type.ElementNullIn != ENullInPolicy::Deny);
        if (allowNull) {
          out += std::format("{}if (s.readNull()) {{ /* ok, stays nullopt */ }}\n", in);
          out += std::format("{}else {{\n", in);
        } else {
          out += std::format("{}if (s.readNull()) {{ s.setError(\"field '{}': null is not allowed for element\"); return false; }}\n", in, ctx);
          out += std::format("{}{{\n", in);
        }
        out += std::format("{}  {}.emplace();\n", in, containerExpr);
        generateVerboseParsePlainLeafElement(out, f, enumNames, std::format("(*{})", containerExpr), ctx, ind + 1);
        out += std::format("{}}}\n", in);
        return;
      }
      case EArrayElementKind::Variant:
        generateVerboseVariantParse(out, f, containerExpr, ctx, enumNames, ind);
        return;
      case EArrayElementKind::Map:
        generateVerboseParseMapElement(out, f, enumNames, containerExpr, ctx, ind);
        return;
      default:
        break;
    }
    // Plain element
    generateVerboseParsePlainLeafElement(out, f, enumNames, containerExpr, ctx, ind);
    return;
  }

  auto &dim = dims[dimIndex];
  std::string idx = std::format("i{}_", dimIndex);

  if (dim.FixedSize > 0) {
    out += std::format("{}if (!s.expectChar('[')) return false;\n", in);
    out += std::format("{}for (size_t {} = 0; {} < {}; {}++) {{\n", in, idx, idx, dim.FixedSize, idx);
    out += std::format("{}  if ({} > 0 && !s.expectChar(',')) return false;\n", in, idx);
    out += std::format("{}  s.skipWhitespace();\n", in);
    generateVerboseNestedArrayParse(out, f, enumNames, dims, dimIndex + 1,
                                    std::format("{}[{}]", containerExpr, idx), ctx, ind + 1);
    out += std::format("{}}}\n", in);
    out += std::format("{}if (!s.expectChar(']')) return false;\n", in);
  } else {
    out += std::format("{}if (!s.expectChar('[')) return false;\n", in);
    out += std::format("{}s.skipWhitespace();\n", in);
    out += std::format("{}if (s.p < s.end && *s.p != ']') {{\n", in);
    out += std::format("{}  for (;;) {{\n", in);
    out += std::format("{}    {}.emplace_back();\n", in, containerExpr);
    generateVerboseNestedArrayParse(out, f, enumNames, dims, dimIndex + 1,
                                     std::format("{}.back()", containerExpr), ctx, ind + 2);
    out += std::format("{}    s.skipWhitespace();\n", in);
    out += std::format("{}    if (s.p < s.end && *s.p == ',') {{ s.p++; s.skipWhitespace(); continue; }}\n", in);
    out += std::format("{}    break;\n", in);
    out += std::format("{}  }}\n", in);
    out += std::format("{}}}\n", in);
    out += std::format("{}if (!s.expectCharNoWs(']')) return false;\n", in);
  }
}

static void generateVerboseVariantParseAttempt(std::string &out, const CVariantAlt &alt,
    size_t altIndex, const std::string &varName,
    const std::unordered_set<std::string> &enumNames, int ind)
{
  std::string in = indent(ind);
  std::string altVar = std::format("__variantAlt{}_value", altIndex);
  CFieldDef tmp = variantAltAsField(alt, altVar);

  out += std::format("{}if (!matched) {{\n", in);
  out += std::format("{}  const char *saved = s.p;\n", in);
  out += std::format("{}  std::remove_reference_t<decltype(std::get<{}>({}))> {}{{}};\n", in, altIndex, varName, altVar);
  out += std::format("{}  bool altOk = ([&]() -> bool {{\n", in);
  generateVerboseParseField(out, tmp, enumNames, -1, ind + 2, false);
  out += std::format("{}    return true;\n", in);
  out += std::format("{}  }})();\n", in);
  out += std::format("{}  if (altOk && __variantBoundary(s.p)) {{\n", in);
  out += std::format("{}    {}.emplace<{}>(std::move({}));\n", in, varName, altIndex, altVar);
  out += std::format("{}    matched = true;\n", in);
  out += std::format("{}  }} else {{\n", in);
  out += std::format("{}    s.p = saved;\n", in);
  out += std::format("{}    if (s.error) {{ s.error->message.clear(); s.error->row = 0; s.error->col = 0; }}\n", in);
  out += std::format("{}  }}\n", in);
  out += std::format("{}}}\n", in);
}

// Generate verbose parse code for a variant field using ordered backtracking.
static void generateVerboseVariantParse(std::string &out, const CFieldDef &f,
    const std::string &varName, const std::string &ctx,
    const std::unordered_set<std::string> &enumNames, int ind)
{
  std::string in = indent(ind);
  out += std::format("{}auto __variantBoundary = [&](const char *p_) {{\n", in);
  out += std::format("{}  return p_ >= s.end || *p_ == ',' || *p_ == ']' || *p_ == '}}' ||\n", in);
  out += std::format("{}         *p_ == ' ' || *p_ == '\\t' || *p_ == '\\r' || *p_ == '\\n';\n", in);
  out += std::format("{}}};\n", in);
  out += std::format("{}bool matched = false;\n", in);
  for (size_t i = 0; i < f.Type.Alternatives.size(); i++)
    generateVerboseVariantParseAttempt(out, f.Type.Alternatives[i], i, varName, enumNames, ind);
  out += std::format("{}if (!matched) {{\n", in);
  out += std::format("{}  s.setError(\"field '{}': could not match any variant alternative\");\n", in, ctx);
  out += std::format("{}  return false;\n", in);
  out += std::format("{}}}\n", in);
}

void generateVerboseParseField(std::string &out, const CFieldDef &f, const std::unordered_set<std::string> &enumNames, int foundBit, int ind, bool pascalCase)
{
  std::string in = indent(ind);
  std::string ctx = f.Name; // field context for error messages (original IDL name)
  std::string cn = fieldCppName(f.Name, pascalCase);

  switch (f.Kind) {
    case EFieldKind::Required:
    case EFieldKind::HasDefault: {
      if (isStructRef(f, enumNames)) {
        out += std::format("{}if (!{}.parseVerboseImpl(s)) return false;\n", in, cn);
        if (foundBit >= 0)
          out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      } else {
        generateVerboseParseScalar(out, f, enumNames, foundBit, ind, ctx, pascalCase);
      }
      break;
    }

    case EFieldKind::OptionalObject: {
      if (fieldAllowsNullInput(f))
        out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      else
        out += std::format("{}if (s.readNull()) {{ s.setError(\"field '{}': null is not allowed\"); return false; }}\n", in, ctx);
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
      if (foundBit >= 0)
        out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::Array: {
      if (!f.Type.InnerDims.empty() || f.Type.ArrayElementKind != EArrayElementKind::Plain) {
        // Multi-dimensional array or complex element kind
        out += std::format("{}if (!s.expectChar('[')) {{ s.error->message = \"field '{}': expected array\"; return false; }}\n", in, ctx);
        out += std::format("{}s.skipWhitespace();\n", in);
        out += std::format("{}if (s.p < s.end && *s.p != ']') {{\n", in);
        out += std::format("{}  for (;;) {{\n", in);
        out += std::format("{}    {}.emplace_back();\n", in, cn);
        generateVerboseNestedArrayParse(out, f, enumNames, f.Type.InnerDims, 0,
                                         std::format("{}.back()", cn), ctx, ind + 2);
        out += std::format("{}    s.skipWhitespace();\n", in);
        out += std::format("{}    if (s.p < s.end && *s.p == ',') {{ s.p++; s.skipWhitespace(); continue; }}\n", in);
        out += std::format("{}    break;\n", in);
        out += std::format("{}  }}\n", in);
        out += std::format("{}}}\n", in);
        out += std::format("{}if (!s.expectCharNoWs(']')) return false;\n", in);
        if (foundBit >= 0)
          out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
        break;
      }

      out += std::format("{}if (!s.expectChar('[')) {{ s.error->message = \"field '{}': expected array\"; return false; }}\n", in, ctx);
      out += std::format("{}s.skipWhitespace();\n", in);
      out += std::format("{}if (s.p < s.end && *s.p != ']') {{\n", in);
      out += std::format("{}  for (;;) {{\n", in);

      if (f.Type.IsUserType) {
        std::string readName = "__" + f.Type.RefName + "Parse";
        out += std::format("{}    {}.emplace_back();\n", in, cn);
        out += std::format("{}    if ({}(s.p, s.end, {}.back()) != JsonReadError::Ok) {{ s.setError(\"field '{}': invalid {} value in array\"); return false; }}\n",
                           in, readName, cn, ctx, f.Type.RefName);
      } else if (isDerivedField(f)) {
        auto wi = getVerboseWireTypeInfo(f.Type.MappedWireType);
        std::string resolveName = "__" + f.Type.RefName + "Resolve";
        out += std::format("{}    {{ {} _tmp; {}.emplace_back();\n", in, wi.cppType, cn);
        out += std::format("{}      if (auto _e = s.{}(_tmp); _e != JsonReadError::Ok) {{ s.formatReadError(_e, \"{}\", \"{}\"); return false; }}\n",
                           in, wi.readMethod, ctx, wi.typeName);
        out += std::format("{}      if (!{}(_tmp, {}.back())) {{ s.setError(\"field '{}': invalid derived value in array\"); return false; }}\n",
                           in, resolveName, cn, ctx);
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
        out += std::format("{}    {{ int64_t _t; if (auto _e = s.readInt64(_t); _e != JsonReadError::Ok) {{ s.formatReadError(_e, \"{}\", \"integer\"); return false; }} "
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
          out += std::format("{}    {{ std::string tmp; if (auto _e = s.{}(tmp); _e != JsonReadError::Ok) {{ s.formatReadError(_e, \"{}\", \"{}\"); return false; }} "
                             "{}.push_back(std::move(tmp)); }}\n", in, readMethod, ctx, scalarTypeName(f.Type.Scalar), cn);
        } else {
          out += std::format("{}    {{ {} tmp; if (auto _e = s.{}(tmp); _e != JsonReadError::Ok) {{ s.formatReadError(_e, \"{}\", \"{}\"); return false; }} "
                             "{}.push_back(tmp); }}\n", in, cppType, readMethod, ctx, scalarTypeName(f.Type.Scalar), cn);
        }
      }

      out += std::format("{}    s.skipWhitespace();\n", in);
      out += std::format("{}    if (s.p < s.end && *s.p == ',') {{ s.p++; s.skipWhitespace(); continue; }}\n", in);
      out += std::format("{}    break;\n", in);
      out += std::format("{}  }}\n", in);
      out += std::format("{}}}\n", in);
      out += std::format("{}if (!s.expectCharNoWs(']')) return false;\n", in);
      if (foundBit >= 0)
        out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::OptionalArray: {
      if (fieldAllowsNullInput(f))
        out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      else
        out += std::format("{}if (s.readNull()) {{ s.setError(\"field '{}': null is not allowed\"); return false; }}\n", in, ctx);
      out += std::format("{}else {{\n", in);
      out += std::format("{}  {}.emplace();\n", in, cn);
      CFieldDef tmp = f;
      tmp.Name = std::format("(*{})", cn);
      tmp.Kind = EFieldKind::Array;
      generateVerboseParseField(out, tmp, enumNames, -1, ind + 1, false);
      out += std::format("{}}}\n", in);
      if (foundBit >= 0)
        out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::FixedArray: {
      out += std::format("{}if (!s.expectChar('[')) return false;\n", in);
      out += std::format("{}for (size_t i_ = 0; i_ < {}; i_++) {{\n", in, f.Type.FixedSize);
      out += std::format("{}  if (i_ > 0 && !s.expectChar(',')) return false;\n", in);
      out += std::format("{}  s.skipWhitespace();\n", in);

      if (!f.Type.InnerDims.empty() || f.Type.ArrayElementKind != EArrayElementKind::Plain) {
        generateVerboseNestedArrayParse(out, f, enumNames, f.Type.InnerDims, 0,
                                         std::format("{}[i_]", cn), ctx, ind + 1);
      } else if (f.Type.IsUserType) {
        std::string readName = "__" + f.Type.RefName + "Parse";
        out += std::format("{}  if ({}(s.p, s.end, {}[i_]) != JsonReadError::Ok) {{ s.setError(\"field '{}': invalid {} value\"); return false; }}\n",
                           in, readName, cn, ctx, f.Type.RefName);
      } else if (isDerivedField(f)) {
        generateVerboseParseMappedValue(out, f.Type.RefName, f.Type.MappedWireType, std::format("{}[i_]", cn), ctx, ind + 1, "return false");
      } else if (isStructRef(f, enumNames)) {
        out += std::format("{}  if (!{}[i_].parseVerboseImpl(s)) return false;\n", in, cn);
      } else if (isEnum(f.Type.RefName, enumNames)) {
        out += std::format("{}  {{ const char *eStr; size_t eLen;\n", in);
        out += std::format("{}    if (!s.readString(eStr, eLen)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }}\n", in, ctx);
        out += std::format("{}    if (!parseE{}(eStr, eLen, {}[i_])) {{ s.setError(\"field '{}': invalid enum value\"); return false; }} }}\n",
                           in, f.Type.RefName, cn, ctx);
      } else if (f.Type.IsScalar && (f.Type.Scalar == EScalarType::Seconds ||
                 f.Type.Scalar == EScalarType::Minutes || f.Type.Scalar == EScalarType::Hours)) {
        std::string chronoType = cppScalarType(f.Type.Scalar);
        out += std::format("{}  {{ int64_t _t; if (auto _e = s.readInt64(_t); _e != JsonReadError::Ok) {{ s.formatReadError(_e, \"{}\", \"integer\"); return false; }} {}[i_] = {}(_t); }}\n",
                           in, ctx, cn, chronoType);
      } else {
        const char *readMethod = nullptr;
        switch (f.Type.Scalar) {
          case EScalarType::String: readMethod = "readStringValue"; break;
          case EScalarType::Bool:   readMethod = "readBool"; break;
          case EScalarType::Int32:  readMethod = "readInt32"; break;
          case EScalarType::Uint32: readMethod = "readUInt32"; break;
          case EScalarType::Int64:  readMethod = "readInt64"; break;
          case EScalarType::Uint64: readMethod = "readUInt64"; break;
          case EScalarType::Double: readMethod = "readDouble"; break;
          default: readMethod = "readInt64"; break;
        }
        out += std::format("{}  if (auto _e = s.{}({}[i_]); _e != JsonReadError::Ok) {{ s.formatReadError(_e, \"{}\", \"{}\"); return false; }}\n",
                           in, readMethod, cn, ctx, scalarTypeName(f.Type.Scalar));
      }

      out += std::format("{}}}\n", in);
      out += std::format("{}if (!s.expectChar(']')) return false;\n", in);
      if (foundBit >= 0)
        out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::OptionalFixedArray: {
      if (fieldAllowsNullInput(f))
        out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      else
        out += std::format("{}if (s.readNull()) {{ s.setError(\"field '{}': null is not allowed\"); return false; }}\n", in, ctx);
      out += std::format("{}else {{\n", in);
      out += std::format("{}  {}.emplace();\n", in, cn);
      CFieldDef tmp = f;
      tmp.Name = std::format("(*{})", cn);
      tmp.Kind = EFieldKind::FixedArray;
      generateVerboseParseField(out, tmp, enumNames, -1, ind + 1, false);
      out += std::format("{}}}\n", in);
      if (foundBit >= 0)
        out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::Map: {
      out += std::format("{}if (!s.expectChar('{{')) {{ s.error->message = \"field '{}': expected object\"; return false; }}\n", in, ctx);
      out += std::format("{}s.skipWhitespace();\n", in);
      out += std::format("{}if (s.p < s.end && *s.p != '}}') {{\n", in);
      out += std::format("{}  for (;;) {{\n", in);
      out += std::format("{}    const char *_mapKey; size_t _mapKeyLen;\n", in);
      out += std::format("{}    if (!s.readString(_mapKey, _mapKeyLen)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }}\n", in, ctx);
      out += std::format("{}    if (!s.expectChar(':')) return false;\n", in);
      out += std::format("{}    s.skipWhitespace();\n", in);
      out += std::format("{}    auto &_mapVal = {}[std::string(_mapKey, _mapKeyLen)];\n", in, cn);

      if (isStructRef(f, enumNames)) {
        out += std::format("{}    if (!_mapVal.parseVerboseImpl(s)) return false;\n", in);
      } else if (isEnum(f.Type.RefName, enumNames)) {
        out += std::format("{}    {{ const char *eStr; size_t eLen;\n", in);
        out += std::format("{}      if (!s.readString(eStr, eLen)) {{ s.error->message = \"field '{}': \" + s.error->message; return false; }}\n", in, ctx);
        out += std::format("{}      if (!parseE{}(eStr, eLen, _mapVal)) {{ s.setError(\"field '{}': invalid enum value\"); return false; }} }}\n",
                           in, f.Type.RefName, ctx);
      } else if (f.Type.IsScalar && (f.Type.Scalar == EScalarType::Seconds ||
                 f.Type.Scalar == EScalarType::Minutes || f.Type.Scalar == EScalarType::Hours)) {
        std::string chronoType = cppScalarType(f.Type.Scalar);
        out += std::format("{}    {{ int64_t _t; if (auto _e = s.readInt64(_t); _e != JsonReadError::Ok) {{ s.formatReadError(_e, \"{}\", \"integer\"); return false; }} _mapVal = {}(_t); }}\n",
                           in, ctx, chronoType);
      } else {
        const char *readMethod = nullptr;
        switch (f.Type.Scalar) {
          case EScalarType::String: readMethod = "readStringValue"; break;
          case EScalarType::Bool:   readMethod = "readBool"; break;
          case EScalarType::Int32:  readMethod = "readInt32"; break;
          case EScalarType::Uint32: readMethod = "readUInt32"; break;
          case EScalarType::Int64:  readMethod = "readInt64"; break;
          case EScalarType::Uint64: readMethod = "readUInt64"; break;
          case EScalarType::Double: readMethod = "readDouble"; break;
          default: readMethod = "readInt64"; break;
        }
        out += std::format("{}    if (auto _e = s.{}(_mapVal); _e != JsonReadError::Ok) {{ s.formatReadError(_e, \"{}\", \"{}\"); return false; }}\n",
                           in, readMethod, ctx, scalarTypeName(f.Type.Scalar));
      }

      out += std::format("{}    s.skipWhitespace();\n", in);
      out += std::format("{}    if (s.p < s.end && *s.p == ',') {{ s.p++; s.skipWhitespace(); continue; }}\n", in);
      out += std::format("{}    break;\n", in);
      out += std::format("{}  }}\n", in);
      out += std::format("{}}}\n", in);
      out += std::format("{}if (!s.expectCharNoWs('}}')) return false;\n", in);
      if (foundBit >= 0)
        out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::OptionalMap: {
      if (fieldAllowsNullInput(f))
        out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      else
        out += std::format("{}if (s.readNull()) {{ s.setError(\"field '{}': null is not allowed\"); return false; }}\n", in, ctx);
      out += std::format("{}else {{\n", in);
      out += std::format("{}  {}.emplace();\n", in, cn);
      CFieldDef tmp = f;
      tmp.Name = std::format("(*{})", cn);
      tmp.Kind = EFieldKind::Map;
      generateVerboseParseField(out, tmp, enumNames, -1, ind + 1, false);
      out += std::format("{}}}\n", in);
      if (foundBit >= 0)
        out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::Variant: {
      out += std::format("{}s.skipWhitespace();\n", in);
      out += std::format("{}if (s.p >= s.end) {{ s.setError(\"field '{}': expected value, got end of input\"); return false; }}\n", in, ctx);
      generateVerboseVariantParse(out, f, cn, ctx, enumNames, ind);
      if (foundBit >= 0)
        out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::OptionalVariant: {
      if (fieldAllowsNullInput(f))
        out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      else
        out += std::format("{}if (s.readNull()) {{ s.setError(\"field '{}': null is not allowed\"); return false; }}\n", in, ctx);
      out += std::format("{}else {{\n", in);
      out += std::format("{}  s.skipWhitespace();\n", in);
      out += std::format("{}  if (s.p >= s.end) {{ s.setError(\"field '{}': expected value\"); return false; }}\n", in, ctx);
      out += std::format("{}  {}.emplace();\n", in, cn);
      generateVerboseVariantParse(out, f, std::format("(*{})", cn), ctx, enumNames, ind + 1);
      out += std::format("{}}}\n", in);
      if (foundBit >= 0)
        out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }
  }
}
