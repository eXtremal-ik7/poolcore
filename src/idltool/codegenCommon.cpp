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

static std::string cppStringLiteral(std::string_view value)
{
  std::string result = "\"";
  for (char ch : value) {
    switch (ch) {
      case '\\': result += "\\\\"; break;
      case '"':  result += "\\\""; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:   result += ch; break;
    }
  }
  result += "\"";
  return result;
}

CSerializeCodeBuilder::CSerializeCodeBuilder(std::string &code)
  : Code_(code)
{
}

void CSerializeCodeBuilder::writeLiteral(int ind, std::string_view literal)
{
  if (literal.empty())
    return;
  if (PendingIndent_ != ind)
    flush();
  PendingIndent_ = ind;
  PendingLiteral_ += literal;
}

void CSerializeCodeBuilder::appendRaw(std::string_view text)
{
  flush();
  Code_ += text;
}

void CSerializeCodeBuilder::flush()
{
  if (PendingIndent_ == -1)
    return;
  Code_ += std::format("{}out.write({});\n", indent(PendingIndent_), cppStringLiteral(PendingLiteral_));
  PendingIndent_ = -1;
  PendingLiteral_.clear();
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

// Build base type name for a single type reference (scalar/struct/enum/mapped)
static std::string baseTypeName(bool isScalar, bool isMapped,
    EScalarType scalar, const std::string &refName,
    const std::string &mappedCppType,
    const std::unordered_set<std::string> &enumNames,
    const std::string &structPrefix)
{
  if (isMapped)
    return mappedCppType;
  if (!refName.empty()) {
    if (enumNames.count(refName))
      return "E" + refName;
    return structPrefix + refName;
  }
  return cppScalarType(scalar);
}

static std::string wrapArrayDims(std::string baseType, const std::vector<CArrayDim> &dims)
{
  for (int i = (int)dims.size() - 1; i >= 0; i--) {
    if (dims[i].FixedSize > 0)
      baseType = std::format("std::array<{}, {}>", baseType, dims[i].FixedSize);
    else
      baseType = std::format("std::vector<{}>", baseType);
  }
  return baseType;
}

CFieldDef variantAltAsField(const CVariantAlt &alt, const std::string &name)
{
  CFieldDef f;
  f.Name = name;
  f.Type.IsScalar = alt.IsScalar;
  f.Type.IsMapped = alt.IsMapped;
  f.Type.Scalar = alt.Scalar;
  f.Type.RefName = alt.RefName;
  f.Type.MappedCppType = alt.MappedCppType;
  f.Type.MappedWireType = alt.MappedWireType;
  if (!alt.Dims.empty()) {
    f.Type.InnerDims.assign(alt.Dims.begin() + 1, alt.Dims.end());
    if (alt.Dims[0].FixedSize > 0) {
      f.Kind = EFieldKind::FixedArray;
      f.Type.FixedSize = alt.Dims[0].FixedSize;
    } else {
      f.Kind = EFieldKind::Array;
    }
  }
  return f;
}

std::string cppVariantAltType(const CVariantAlt &alt,
    const std::unordered_set<std::string> &enumNames,
    const std::string &structPrefix)
{
  if (!alt.Dims.empty()) {
    return wrapArrayDims(baseTypeName(alt.IsScalar, alt.IsMapped, alt.Scalar, alt.RefName,
                                      alt.MappedCppType, enumNames, structPrefix),
                         alt.Dims);
  }
  return baseTypeName(alt.IsScalar, alt.IsMapped, alt.Scalar, alt.RefName,
                      alt.MappedCppType, enumNames, structPrefix);
}

// Build the variant type from alternatives
static std::string variantCppType(const std::vector<CVariantAlt> &alts,
    const std::unordered_set<std::string> &enumNames,
    const std::string &structPrefix)
{
  std::string result = "std::variant<";
  for (size_t i = 0; i < alts.size(); i++) {
    if (i) result += ", ";
    result += cppVariantAltType(alts[i], enumNames, structPrefix);
  }
  result += ">";
  return result;
}

std::string cppFieldType(const CFieldDef &f, const std::unordered_set<std::string> &enumNames, const std::string &structPrefix)
{
  // Variant types
  if (!f.Type.Alternatives.empty()) {
    std::string vt = variantCppType(f.Type.Alternatives, enumNames, structPrefix);
    switch (f.Kind) {
      case EFieldKind::Variant:         return vt;
      case EFieldKind::OptionalVariant:
      case EFieldKind::NullableVariant: return std::format("std::optional<{}>", vt);
      default: return vt;
    }
  }

  std::string base = baseTypeName(f.Type.IsScalar, f.Type.IsMapped, f.Type.Scalar,
                                   f.Type.RefName, f.Type.MappedCppType, enumNames, structPrefix);

  // Multi-dimensional: wrap from innermost to outermost
  if (!f.Type.InnerDims.empty()) {
    for (int d = (int)f.Type.InnerDims.size() - 1; d >= 0; d--) {
      if (f.Type.InnerDims[d].FixedSize > 0)
        base = std::format("std::array<{}, {}>", base, f.Type.InnerDims[d].FixedSize);
      else
        base = std::format("std::vector<{}>", base);
    }
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
    case EFieldKind::FixedArray:
      return std::format("std::array<{}, {}>", base, f.Type.FixedSize);
    case EFieldKind::OptionalFixedArray:
    case EFieldKind::NullableFixedArray:
      return std::format("std::optional<std::array<{}, {}>>", base, f.Type.FixedSize);
    default:
      return base;
  }
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

void emitSerializeValue(CSerializeCodeBuilder &code, const CFieldDef &f,
                        const std::string &valueName,
                        const std::unordered_set<std::string> &enumNames,
                        int ind)
{
  std::string in = indent(ind);
  if (isEnum(f.Type.RefName, enumNames)) {
    code.writeLiteral(ind, "\"");
    code.appendRaw(std::format("{}out.write(E{}ToString({}));\n", in, f.Type.RefName, valueName));
    code.writeLiteral(ind, "\"");
    return;
  }
  switch (f.Type.Scalar) {
    case EScalarType::String:
      code.appendRaw(std::format("{}jsonWriteString(out, {});\n", in, valueName)); break;
    case EScalarType::Bool:
      code.appendRaw(std::format("{}jsonWriteBool(out, {});\n", in, valueName)); break;
    case EScalarType::Int32: case EScalarType::Int64:
      code.appendRaw(std::format("{}jsonWriteInt(out, {});\n", in, valueName)); break;
    case EScalarType::Uint32: case EScalarType::Uint64:
      code.appendRaw(std::format("{}jsonWriteUInt(out, {});\n", in, valueName)); break;
    case EScalarType::Double:
      code.appendRaw(std::format("{}jsonWriteDouble(out, {});\n", in, valueName)); break;
    case EScalarType::Seconds: case EScalarType::Minutes: case EScalarType::Hours:
      code.appendRaw(std::format("{}jsonWriteInt(out, {}.count());\n", in, valueName)); break;
  }
}

bool isMappedField(const CFieldDef &f)
{
  return f.Type.IsMapped;
}

void emitSerializeArrayElem(CSerializeCodeBuilder &code, const CFieldDef &f,
                            const std::string &valueName,
                            const std::unordered_set<std::string> &enumNames,
                            int ind)
{
  if (isStructRef(f, enumNames)) {
    code.appendRaw(std::format("{}{}.serialize(out);\n", indent(ind), valueName));
    return;
  }
  emitSerializeValue(code, f, valueName, enumNames, ind);
}

static void emitMappedInlineValueSerialize(CSerializeCodeBuilder &code,
                                           const std::string &mappedTypeName,
                                           const std::string &mappedWireType,
                                           const std::string &valueName,
                                           int ind)
{
  std::string in = indent(ind);
  std::string formatFunc = "__" + mappedTypeName + "Format";
  if (mappedWireType == "string") {
    code.appendRaw(std::format("{}jsonWriteString(out, {}({}));\n", in, formatFunc, valueName));
  } else if (mappedWireType == "bool") {
    code.appendRaw(std::format("{}jsonWriteBool(out, {}({}));\n", in, formatFunc, valueName));
  } else if (mappedWireType == "double") {
    code.appendRaw(std::format("{}jsonWriteDouble(out, {}({}));\n", in, formatFunc, valueName));
  } else if (mappedWireType == "uint32" || mappedWireType == "uint64") {
    code.appendRaw(std::format("{}jsonWriteUInt(out, {}({}));\n", in, formatFunc, valueName));
  } else {
    code.appendRaw(std::format("{}jsonWriteInt(out, {}({}));\n", in, formatFunc, valueName));
  }
}

static void emitMappedInlineNestedArraySerialize(CSerializeCodeBuilder &code,
                                                 const CFieldDef &f,
                                                 const std::vector<CArrayDim> &dims, int dimIndex,
                                                 const std::string &valueName,
                                                 int ind)
{
  std::string in = indent(ind);
  if (dimIndex >= (int)dims.size()) {
    emitMappedInlineValueSerialize(code, f.Type.RefName, f.Type.MappedWireType, valueName, ind);
    return;
  }

  code.writeLiteral(ind, "[");
  std::string idx = std::format("i{}_", dimIndex);
  code.appendRaw(std::format("{}for (size_t {} = 0; {} < {}.size(); {}++) {{\n", in, idx, idx, valueName, idx));
  code.appendRaw(std::format("{}  if ({}) out.write(',');\n", in, idx));
  emitMappedInlineNestedArraySerialize(code, f, dims, dimIndex + 1,
                                       std::format("{}[{}]", valueName, idx), ind + 1);
  code.appendRaw(std::format("{}}}\n", in));
  code.writeLiteral(ind, "]");
}

void emitSerializeExpr(CSerializeCodeBuilder &code, const CFieldDef &f,
                       const std::string &valueName,
                       const std::unordered_set<std::string> &enumNames,
                       int ind)
{
  std::string in = indent(ind);
  if (f.Kind == EFieldKind::Array || f.Kind == EFieldKind::FixedArray) {
    code.writeLiteral(ind, "[");
    code.appendRaw(std::format("{}for (size_t i_ = 0; i_ < {}.size(); i_++) {{\n", in, valueName));
    code.appendRaw(std::format("{}  if (i_) out.write(',');\n", in));
    if (isMappedField(f)) {
      if (f.Type.InnerDims.empty()) {
        emitMappedInlineValueSerialize(code, f.Type.RefName, f.Type.MappedWireType,
                                       std::format("{}[i_]", valueName), ind + 1);
      } else {
        emitMappedInlineNestedArraySerialize(code, f, f.Type.InnerDims, 0,
                                             std::format("{}[i_]", valueName), ind + 1);
      }
    } else if (f.Type.InnerDims.empty()) {
      emitSerializeArrayElem(code, f, std::format("{}[i_]", valueName), enumNames, ind + 1);
    } else {
      emitNestedArraySerialize(code, f, f.Type.InnerDims, 0, std::format("{}[i_]", valueName), enumNames, ind + 1);
    }
    code.appendRaw(std::format("{}}}\n", in));
    code.writeLiteral(ind, "]");
    return;
  }

  if (isMappedField(f)) {
    emitMappedInlineValueSerialize(code, f.Type.RefName, f.Type.MappedWireType, valueName, ind);
    return;
  }
  if (isStructRef(f, enumNames)) {
    code.appendRaw(std::format("{}{}.serialize(out);\n", in, valueName));
    return;
  }
  emitSerializeValue(code, f, valueName, enumNames, ind);
}

// Serialize a variant value (already extracted from the field)
void emitVariantSerialize(CSerializeCodeBuilder &code, const CFieldDef &f,
                           const std::string &valueName,
                           const std::unordered_set<std::string> &enumNames,
                           int ind, const std::string &structPrefix)
{
  std::string in = indent(ind);
  code.appendRaw(std::format("{}switch (({}).index()) {{\n", in, valueName));
  for (size_t i = 0; i < f.Type.Alternatives.size(); i++) {
    auto &alt = f.Type.Alternatives[i];
    CFieldDef tmp = variantAltAsField(alt);
    code.appendRaw(std::format("{}  case {}: {{\n", in, i));
    std::string getter = std::format("std::get<{}>({})", i, valueName);
    emitSerializeExpr(code, tmp, getter, enumNames, ind + 2);
    code.appendRaw(std::format("{}    break;\n", in));
    code.appendRaw(std::format("{}  }}\n", in));
  }
  code.appendRaw(std::format("{}}}\n", in));
}

// Serialize nested multi-dimensional arrays
void emitNestedArraySerialize(CSerializeCodeBuilder &code, const CFieldDef &f,
                               const std::vector<CArrayDim> &dims, int dimIndex,
                               const std::string &valueName,
                               const std::unordered_set<std::string> &enumNames,
                               int ind)
{
  std::string in = indent(ind);
  if (dimIndex >= (int)dims.size()) {
    // Leaf: serialize element
    emitSerializeArrayElem(code, f, valueName, enumNames, ind);
    return;
  }

  code.writeLiteral(ind, "[");
  std::string idx = std::format("i{}_", dimIndex);
  code.appendRaw(std::format("{}for (size_t {} = 0; {} < {}.size(); {}++) {{\n", in, idx, idx, valueName, idx));
  code.appendRaw(std::format("{}  if ({}) out.write(',');\n", in, idx));
  std::string elemExpr = std::format("{}[{}]", valueName, idx);
  emitNestedArraySerialize(code, f, dims, dimIndex + 1, elemExpr, enumNames, ind + 1);
  code.appendRaw(std::format("{}}}\n", in));
  code.writeLiteral(ind, "]");
}

void generateSerializeField(CSerializeCodeBuilder &code, const CFieldDef &f, const std::unordered_set<std::string> &enumNames, int ind, bool &first, bool pascalCase, bool useRuntimeComma, std::string_view commaVar)
{
  std::string in = indent(ind);
  std::string cn = fieldCppName(f.Name, pascalCase);
  auto emitKey = [&](const std::string &jsonKey, int keyInd = -1) {
    if (keyInd == -1)
      keyInd = ind;
    if (useRuntimeComma) {
      std::string keyIndent = indent(keyInd);
      code.appendRaw(std::format("{}if ({}) out.write(',');\n", keyIndent, commaVar));
      code.appendRaw(std::format("{}{} = true;\n", keyIndent, commaVar));
      code.writeLiteral(keyInd, std::format("\"{}\":", jsonKey));
      return;
    }

    if (!first)
      code.writeLiteral(keyInd, ",");
    first = false;
    code.writeLiteral(keyInd, std::format("\"{}\":", jsonKey));
  };

  // Mapped field: write empty string in standard serialize
  if (isMappedField(f)) {
    emitKey(f.Name);
    code.appendRaw(std::format("{}jsonWriteString(out, \"\");\n", in));
    return;
  }

  switch (f.Kind) {
    case EFieldKind::Required: {
      emitKey(f.Name);
      if (isStructRef(f, enumNames)) {
        code.appendRaw(std::format("{}{}.serialize(out);\n", in, cn));
      } else {
        emitSerializeValue(code, f, cn, enumNames, ind);
      }
      break;
    }

    case EFieldKind::Optional: {
      std::string def = cppDefault(f, enumNames, pascalCase);
      if (def.empty()) {
        emitKey(f.Name);
        if (isStructRef(f, enumNames)) {
          code.appendRaw(std::format("{}{}.serialize(out);\n", in, cn));
        } else {
          emitSerializeValue(code, f, cn, enumNames, ind);
        }
        break;
      }

      code.appendRaw(std::format("{}if ({} != {}) {{\n", in, cn, def));
      emitKey(f.Name, ind + 1);
      if (isStructRef(f, enumNames)) {
        code.appendRaw(std::format("{}{}.serialize(out);\n", indent(ind + 1), cn));
      } else {
        emitSerializeValue(code, f, cn, enumNames, ind + 1);
      }
      code.appendRaw(std::format("{}}}\n", in));
      break;
    }

    case EFieldKind::OptionalObject: {
      code.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
      emitKey(f.Name, ind + 1);
      if (isStructRef(f, enumNames)) {
        code.appendRaw(std::format("{}{}->serialize(out);\n", indent(ind + 1), cn));
      } else {
        emitSerializeValue(code, f, std::format("*{}", cn), enumNames, ind + 1);
      }
      code.appendRaw(std::format("{}}}\n", in));
      break;
    }

    case EFieldKind::NullableObject: {
      emitKey(f.Name);
      code.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
      if (isStructRef(f, enumNames)) {
        code.appendRaw(std::format("{}{}->serialize(out);\n", indent(ind + 1), cn));
      } else {
        emitSerializeValue(code, f, std::format("*{}", cn), enumNames, ind + 1);
      }
      code.appendRaw(std::format("{}}} else {{\n", in));
      code.writeLiteral(ind + 1, "null");
      code.appendRaw(std::format("{}}}\n", in));
      break;
    }

    case EFieldKind::Array: {
      emitKey(f.Name);
      code.writeLiteral(ind, "[");
      code.appendRaw(std::format("{}for (size_t i_ = 0; i_ < {}.size(); i_++) {{\n", in, cn));
      code.appendRaw(std::format("{}  if (i_) out.write(',');\n", in));
      if (f.Type.InnerDims.empty())
        emitSerializeArrayElem(code, f, std::format("{}[i_]", cn), enumNames, ind + 1);
      else
        emitNestedArraySerialize(code, f, f.Type.InnerDims, 0, std::format("{}[i_]", cn), enumNames, ind + 1);
      code.appendRaw(std::format("{}}}\n", in));
      code.writeLiteral(ind, "]");
      break;
    }

    case EFieldKind::OptionalArray: {
      code.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
      emitKey(f.Name, ind + 1);
      code.writeLiteral(ind + 1, "[");
      code.appendRaw(std::format("{}for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", indent(ind + 1), cn));
      code.appendRaw(std::format("{}  if (i_) out.write(',');\n", indent(ind + 1)));
      if (f.Type.InnerDims.empty()) {
        emitSerializeArrayElem(code, f, std::format("(*{})[i_]", cn), enumNames, ind + 2);
      } else {
        emitNestedArraySerialize(code, f, f.Type.InnerDims, 0, std::format("(*{})[i_]", cn), enumNames, ind + 2);
      }
      code.appendRaw(std::format("{}}}\n", indent(ind + 1)));
      code.writeLiteral(ind + 1, "]");
      code.appendRaw(std::format("{}}}\n", in));
      break;
    }

    case EFieldKind::NullableArray: {
      emitKey(f.Name);
      code.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
      code.writeLiteral(ind + 1, "[");
      code.appendRaw(std::format("{}for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", indent(ind + 1), cn));
      code.appendRaw(std::format("{}  if (i_) out.write(',');\n", indent(ind + 1)));
      if (f.Type.InnerDims.empty()) {
        emitSerializeArrayElem(code, f, std::format("(*{})[i_]", cn), enumNames, ind + 2);
      } else {
        emitNestedArraySerialize(code, f, f.Type.InnerDims, 0, std::format("(*{})[i_]", cn), enumNames, ind + 2);
      }
      code.appendRaw(std::format("{}}}\n", indent(ind + 1)));
      code.writeLiteral(ind + 1, "]");
      code.appendRaw(std::format("{}}} else {{\n", in));
      code.writeLiteral(ind + 1, "null");
      code.appendRaw(std::format("{}}}\n", in));
      break;
    }

    case EFieldKind::FixedArray: {
      emitKey(f.Name);
      code.writeLiteral(ind, "[");
      code.appendRaw(std::format("{}for (size_t i_ = 0; i_ < {}.size(); i_++) {{\n", in, cn));
      code.appendRaw(std::format("{}  if (i_) out.write(',');\n", in));
      if (f.Type.InnerDims.empty()) {
        emitSerializeArrayElem(code, f, std::format("{}[i_]", cn), enumNames, ind + 1);
      } else {
        emitNestedArraySerialize(code, f, f.Type.InnerDims, 0, std::format("{}[i_]", cn), enumNames, ind + 1);
      }
      code.appendRaw(std::format("{}}}\n", in));
      code.writeLiteral(ind, "]");
      break;
    }

    case EFieldKind::OptionalFixedArray: {
      code.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
      emitKey(f.Name, ind + 1);
      code.writeLiteral(ind + 1, "[");
      code.appendRaw(std::format("{}for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", indent(ind + 1), cn));
      code.appendRaw(std::format("{}  if (i_) out.write(',');\n", indent(ind + 1)));
      if (f.Type.InnerDims.empty()) {
        emitSerializeArrayElem(code, f, std::format("(*{})[i_]", cn), enumNames, ind + 2);
      } else {
        emitNestedArraySerialize(code, f, f.Type.InnerDims, 0, std::format("(*{})[i_]", cn), enumNames, ind + 2);
      }
      code.appendRaw(std::format("{}}}\n", indent(ind + 1)));
      code.writeLiteral(ind + 1, "]");
      code.appendRaw(std::format("{}}}\n", in));
      break;
    }

    case EFieldKind::NullableFixedArray: {
      emitKey(f.Name);
      code.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
      code.writeLiteral(ind + 1, "[");
      code.appendRaw(std::format("{}for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", indent(ind + 1), cn));
      code.appendRaw(std::format("{}  if (i_) out.write(',');\n", indent(ind + 1)));
      if (f.Type.InnerDims.empty()) {
        emitSerializeArrayElem(code, f, std::format("(*{})[i_]", cn), enumNames, ind + 2);
      } else {
        emitNestedArraySerialize(code, f, f.Type.InnerDims, 0, std::format("(*{})[i_]", cn), enumNames, ind + 2);
      }
      code.appendRaw(std::format("{}}}\n", indent(ind + 1)));
      code.writeLiteral(ind + 1, "]");
      code.appendRaw(std::format("{}}} else {{\n", in));
      code.writeLiteral(ind + 1, "null");
      code.appendRaw(std::format("{}}}\n", in));
      break;
    }

    case EFieldKind::Variant: {
      emitKey(f.Name);
      emitVariantSerialize(code, f, cn, enumNames, ind);
      break;
    }

    case EFieldKind::OptionalVariant: {
      code.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
      emitKey(f.Name, ind + 1);
      emitVariantSerialize(code, f, std::format("*{}", cn), enumNames, ind + 1);
      code.appendRaw(std::format("{}}}\n", in));
      break;
    }

    case EFieldKind::NullableVariant: {
      emitKey(f.Name);
      code.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
      emitVariantSerialize(code, f, std::format("*{}", cn), enumNames, ind + 1);
      code.appendRaw(std::format("{}}} else {{\n", in));
      code.writeLiteral(ind + 1, "null");
      code.appendRaw(std::format("{}}}\n", in));
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
    if (*p == '-') return false;
    char *ep;
    out = strtoull(p, &ep, 10);
    if (ep == p) return false;
    p = ep;
    return true;
  }

  bool readInt32(int32_t &out) {
    int64_t v;
    if (!readInt64(v)) return false;
    if (v < INT32_MIN || v > INT32_MAX) return false;
    out = (int32_t)v;
    return true;
  }

  bool readUInt32(uint32_t &out) {
    uint64_t v;
    if (!readUInt64(v)) return false;
    if (v > UINT32_MAX) return false;
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
