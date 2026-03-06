#include "codegen.h"
#include "codegenCommon.h"
#include "codegenVerbose.h"
#include <cstdio>
#include <cstdint>
#include <format>
#include <unordered_set>
#include <unordered_map>

// ============================================================================
// Context analysis
// ============================================================================

// Per-field capture info: how a field participates in context capture
struct CFieldCaptureInfo {
  enum Kind { None, MappedDirect, MappedInline, NestedStruct };
  Kind kind = None;
  std::string CaptureFieldName;  // field name in the Capture struct
  std::string MappedTypeName;    // for MappedDirect/MappedInline: which mapped type (for resolve/format function names)
  std::string MappedWireType;    // for MappedInline: JSON wire type (e.g. "int64", "string")
  // For NestedStruct: how to pass context to child
  std::string ChildCtxParamName;   // external param name (e.g., "moneyCtx")
  bool ChildCtxIsVector = false;   // true = use [i_] indexing in array loop
};

// Per-struct context analysis result
struct CContextAnalysis {
  struct ContextNeed {
    std::string MappedTypeName;   // IDL name, e.g. "money"
    std::string CppContextType;   // e.g. "uint32_t"
    std::string ParamName;        // e.g. "moneyCtx"
    bool IsVector = false;        // true = pass as const std::vector<T>&
  };
  std::vector<ContextNeed> Needs;

  // Parallel to s.Fields
  std::vector<CFieldCaptureInfo> FieldCapture;

  bool hasContext() const { return !Needs.empty(); }

  std::string contextParams() const {
    std::string result;
    for (auto &n : Needs) {
      if (!result.empty()) result += ", ";
      if (n.IsVector)
        result += "const std::vector<" + n.CppContextType + "> &" + n.ParamName;
      else
        result += n.CppContextType + " " + n.ParamName;
    }
    return result;
  }

  std::string contextArgs() const {
    std::string result;
    for (auto &n : Needs) {
      if (!result.empty()) result += ", ";
      result += n.ParamName;
    }
    return result;
  }
};

static CContextAnalysis analyzeContext(const CStructDef &s, bool pascalCase,
    const std::unordered_map<std::string, const CMappedTypeDef *> &mappedTypes,
    const std::unordered_map<std::string, CContextAnalysis> &childAnalysis)
{
  CContextAnalysis a;
  a.FieldCapture.resize(s.Fields.size());

  std::unordered_set<std::string> contextDeclSet(s.ContextDecls.begin(), s.ContextDecls.end());
  std::unordered_set<std::string> needsSet;

  // Add needs from context declarations (direct mapped fields)
  for (auto &cd : s.ContextDecls) {
    auto it = mappedTypes.find(cd);
    if (it != mappedTypes.end() && it->second->ContextType) {
      if (needsSet.insert(cd).second) {
        a.Needs.push_back({cd, cppScalarType(*it->second->ContextType), cd + "Ctx"});
      }
    }
  }

  // Mark direct mapped fields with context
  for (size_t i = 0; i < s.Fields.size(); i++) {
    auto &f = s.Fields[i];
    if (f.Type.IsMapped && contextDeclSet.count(f.Type.RefName)) {
      a.FieldCapture[i].kind = CFieldCaptureInfo::MappedDirect;
      a.FieldCapture[i].CaptureFieldName = fieldCppName(f.Name, pascalCase);
      a.FieldCapture[i].MappedTypeName = f.Type.RefName;
    }
  }

  // Mark context-free mapped fields (inline resolve/format, no capture needed)
  for (size_t i = 0; i < s.Fields.size(); i++) {
    auto &f = s.Fields[i];
    if (f.Type.IsMapped && a.FieldCapture[i].kind == CFieldCaptureInfo::None) {
      auto it = mappedTypes.find(f.Type.RefName);
      if (it != mappedTypes.end() && !it->second->ContextType) {
        a.FieldCapture[i].kind = CFieldCaptureInfo::MappedInline;
        a.FieldCapture[i].CaptureFieldName = fieldCppName(f.Name, pascalCase);
        a.FieldCapture[i].MappedTypeName = f.Type.RefName;
        a.FieldCapture[i].MappedWireType = it->second->JsonWireType;
      }
    }
  }

  // Check nested struct/array fields for context propagation
  for (size_t i = 0; i < s.Fields.size(); i++) {
    auto &f = s.Fields[i];
    if (!f.Type.RefName.empty() && !f.Type.IsMapped && !f.Type.IsScalar) {
      auto cit = childAnalysis.find(f.Type.RefName);
      if (cit != childAnalysis.end() && cit->second.hasContext()) {
        a.FieldCapture[i].kind = CFieldCaptureInfo::NestedStruct;
        a.FieldCapture[i].CaptureFieldName = fieldCppName(f.Name, pascalCase);

        bool isArray = (f.Kind == EFieldKind::Array || f.Kind == EFieldKind::OptionalArray || f.Kind == EFieldKind::NullableArray);

        for (auto &need : cit->second.Needs) {
          // If parent declares the same context, broadcast scalar to array children
          bool parentDeclares = contextDeclSet.count(need.MappedTypeName) > 0;
          bool promoteToVector = isArray && !parentDeclares;

          a.FieldCapture[i].ChildCtxParamName = need.ParamName;
          a.FieldCapture[i].ChildCtxIsVector = promoteToVector;
          if (needsSet.insert(need.MappedTypeName).second) {
            auto inheritedNeed = need;
            if (promoteToVector)
              inheritedNeed.IsVector = true;
            a.Needs.push_back(inheritedNeed);
          } else if (promoteToVector) {
            // Promote existing need to vector
            for (auto &n : a.Needs) {
              if (n.MappedTypeName == need.MappedTypeName)
                n.IsVector = true;
            }
          }
        }
      }
    }
  }

  return a;
}

// Generate the expression to pass context to a nested child
static std::string childCtxExpr(const CFieldCaptureInfo &ci,
                                 const std::string &indexVar)
{
  if (ci.ChildCtxIsVector && !indexVar.empty())
    return ci.ChildCtxParamName + "[" + indexVar + "]";
  return ci.ChildCtxParamName;
}

// Wire type info for context-free mapped types
struct WireTypeInfo {
  const char *readMethod;  // JsonScanner method
  const char *writeFunc;   // jsonWrite* function
  const char *cppType;     // C++ wire type
};

static WireTypeInfo getWireTypeInfo(const std::string &wireType) {
  if (wireType == "string") return {"readStringValue", "jsonWriteString", "std::string"};
  if (wireType == "int64")  return {"readInt64", "jsonWriteInt", "int64_t"};
  if (wireType == "uint64") return {"readUInt64", "jsonWriteInt", "uint64_t"};
  if (wireType == "int32")  return {"readInt32", "jsonWriteInt", "int32_t"};
  if (wireType == "uint32") return {"readUInt32", "jsonWriteInt", "uint32_t"};
  if (wireType == "double") return {"readDouble", "jsonWriteDouble", "double"};
  if (wireType == "bool")   return {"readBool", "jsonWriteBool", "bool"};
  return {"readStringValue", "jsonWriteString", "std::string"};
}

// ============================================================================
// Parse generation
// ============================================================================

static void generateParseScalar(std::string &out, const CFieldDef &f,
    const std::unordered_set<std::string> &enumNames, int foundBit, int ind,
    bool pascalCase, const CFieldCaptureInfo *ci = nullptr)
{
  std::string in = indent(ind);
  std::string cn = fieldCppName(f.Name, pascalCase);

  // Mapped field
  if (isMappedField(f)) {
    if (ci && ci->kind == CFieldCaptureInfo::MappedDirect) {
      out += std::format("{}if (capture) {{\n", in);
      out += std::format("{}  if (!s.readStringValue(capture->{})) valid = false;\n", in, ci->CaptureFieldName);
      if (foundBit >= 0)
        out += std::format("{}  else found |= (uint64_t)1 << {};\n", in, foundBit);
      out += std::format("{}}} else {{\n", in);
      out += std::format("{}  if (!s.skipValue()) valid = false;\n", in);
      if (foundBit >= 0)
        out += std::format("{}  else found |= (uint64_t)1 << {};\n", in, foundBit);
      out += std::format("{}}}\n", in);
    } else if (ci && ci->kind == CFieldCaptureInfo::MappedInline) {
      auto wi = getWireTypeInfo(ci->MappedWireType);
      std::string resolveFunc = "__" + ci->MappedTypeName + "Resolve";
      out += std::format("{}{{ {} _tmp; if (!s.{}(_tmp) || !{}(_tmp, {})) valid = false;\n",
                         in, wi.cppType, wi.readMethod, resolveFunc, cn);
      if (foundBit >= 0)
        out += std::format("{}  else found |= (uint64_t)1 << {};\n", in, foundBit);
      out += std::format("{}}}\n", in);
    } else {
      out += std::format("{}if (!s.skipValue()) valid = false;\n", in);
      if (foundBit >= 0)
        out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
    }
    return;
  }

  if (isEnum(f.Type.RefName, enumNames)) {
    out += std::format("{}if (!([&]() {{ const char *eStr; size_t eLen; "
                       "return s.readString(eStr, eLen) && parseE{}(eStr, eLen, {}); }}())) valid = false;\n",
                       in, f.Type.RefName, cn);
    if (foundBit >= 0)
      out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
    return;
  }

  // Chrono types
  if (f.Type.IsScalar && (f.Type.Scalar == EScalarType::Seconds ||
      f.Type.Scalar == EScalarType::Minutes || f.Type.Scalar == EScalarType::Hours)) {
    std::string chronoType = cppScalarType(f.Type.Scalar);
    out += std::format("{}{{ int64_t _t; if (!s.readInt64(_t)) valid = false;\n", in);
    out += std::format("{}  else {{ {} = {}(_t);\n", in, cn, chronoType);
    if (foundBit >= 0)
      out += std::format("{}    found |= (uint64_t)1 << {};\n", in, foundBit);
    out += std::format("{}  }} }}\n", in);
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

  out += std::format("{}if (!s.{}({})) valid = false;\n", in, readMethod, cn);
  if (foundBit >= 0)
    out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
}

static void generateParseField(std::string &out, const CFieldDef &f,
    const std::unordered_set<std::string> &enumNames, int foundBit, int ind,
    bool pascalCase, const CFieldCaptureInfo *ci = nullptr)
{
  std::string in = indent(ind);
  std::string cn = fieldCppName(f.Name, pascalCase);

  switch (f.Kind) {
    case EFieldKind::Required:
    case EFieldKind::Optional: {
      if (isStructRef(f, enumNames)) {
        if (ci && ci->kind == CFieldCaptureInfo::NestedStruct) {
          out += std::format("{}if (!{}.parseImpl(s, capture ? &capture->{} : nullptr)) valid = false;\n",
                             in, cn, ci->CaptureFieldName);
        } else {
          out += std::format("{}if (!{}.parseImpl(s)) valid = false;\n", in, cn);
        }
        if (foundBit >= 0)
          out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
      } else {
        generateParseScalar(out, f, enumNames, foundBit, ind, pascalCase,
                            (isMappedField(f) && ci) ? ci : nullptr);
      }
      break;
    }

    case EFieldKind::OptionalObject:
    case EFieldKind::NullableObject: {
      out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      if (isStructRef(f, enumNames)) {
        if (ci && ci->kind == CFieldCaptureInfo::NestedStruct) {
          out += std::format("{}else if (!{}.emplace().parseImpl(s, capture ? &capture->{} : nullptr)) valid = false;\n",
                             in, cn, ci->CaptureFieldName);
        } else {
          out += std::format("{}else if (!{}.emplace().parseImpl(s)) valid = false;\n", in, cn);
        }
      } else {
        out += std::format("{}else {{\n", in);
        out += std::format("{}  {}.emplace();\n", in, cn);
        CFieldDef tmp = f;
        tmp.Name = std::format("(*{})", cn);
        tmp.Kind = EFieldKind::Required;
        generateParseScalar(out, tmp, enumNames, -1, ind + 1, false, (isMappedField(f) && ci) ? ci : nullptr);
        out += std::format("{}}}\n", in);
      }
      break;
    }

    case EFieldKind::Array: {
      out += std::format("{}if (!s.expectChar('[')) {{ valid = false; break; }}\n", in);
      out += std::format("{}s.skipWhitespace();\n", in);
      out += std::format("{}if (s.p < s.end && *s.p != ']') {{\n", in);
      out += std::format("{}  for (;;) {{\n", in);

      if (isStructRef(f, enumNames)) {
        if (ci && ci->kind == CFieldCaptureInfo::NestedStruct) {
          out += std::format("{}    if (capture) capture->{}.emplace_back();\n", in, ci->CaptureFieldName);
          out += std::format("{}    if (!{}.emplace_back().parseImpl(s, capture ? &capture->{}.back() : nullptr)) {{ valid = false; break; }}\n",
                             in, cn, ci->CaptureFieldName);
        } else {
          out += std::format("{}    if (!{}.emplace_back().parseImpl(s)) {{ valid = false; break; }}\n", in, cn);
        }
      } else if (isEnum(f.Type.RefName, enumNames)) {
        out += std::format("{}    {{\n", in);
        out += std::format("{}      const char *eStr; size_t eLen;\n", in);
        out += std::format("{}      {}.emplace_back();\n", in, cn);
        out += std::format("{}      if (!s.readString(eStr, eLen) || !parseE{}(eStr, eLen, {}.back())) "
                           "{{ valid = false; break; }}\n", in, f.Type.RefName, cn);
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
          default: cppType = "int64_t"; readMethod = "readInt64"; break;
        }
        if (f.Type.Scalar == EScalarType::String) {
          out += std::format("{}    {{ std::string tmp; if (!s.{}(tmp)) {{ valid = false; break; }} "
                             "{}.push_back(std::move(tmp)); }}\n", in, readMethod, cn);
        } else {
          out += std::format("{}    {{ {} tmp; if (!s.{}(tmp)) {{ valid = false; break; }} "
                             "{}.push_back(tmp); }}\n", in, cppType, readMethod, cn);
        }
      }

      out += std::format("{}    s.skipWhitespace();\n", in);
      out += std::format("{}    if (s.p < s.end && *s.p == ',') {{ s.p++; continue; }}\n", in);
      out += std::format("{}    break;\n", in);
      out += std::format("{}  }}\n", in);
      out += std::format("{}}}\n", in);
      out += std::format("{}if (!s.expectChar(']')) valid = false;\n", in);
      if (foundBit >= 0)
        out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
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
      generateParseField(out, tmp, enumNames, -1, ind + 1, false, ci);
      out += std::format("{}}}\n", in);
      break;
    }
  }
}

// Generate the parse loop body
static void generateParseBody(std::string &out, const CStructDef &s,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts, const CPerfectHash &ph,
    const std::unordered_map<uint32_t, size_t> &hashToIndex,
    const std::unordered_map<size_t, int> &fieldBitMap,
    int requiredCount,
    const std::vector<CFieldCaptureInfo> *fieldCapture = nullptr)
{
  out += "  if (!s.expectChar('{')) return false;\n";
  out += "  bool valid = true;\n";
  if (requiredCount > 0)
    out += "  uint64_t found = 0;\n";
  out += "  s.skipWhitespace();\n";
  out += "  if (s.p < s.end && *s.p != '}') {\n";
  out += "    for (;;) {\n";
  out += "      const char *key;\n";
  out += "      size_t keyLen;\n";

  if (s.Fields.empty()) {
    out += "      if (!s.readString(key, keyLen)) return false;\n";
    out += "      if (!s.expectChar(':')) return false;\n";
    out += "      return false;\n";
  } else {
    out += "      uint32_t keyHash;\n";
    out += std::format("      if (!s.readStringHash(key, keyLen, keyHash, {}, {})) return false;\n",
                       ph.Seed, ph.Mult);
    out += "      if (!s.expectChar(':')) return false;\n";
    out += std::format("      switch (keyHash % {}) {{\n", ph.Mod);

    for (uint32_t h = 0; h < ph.Mod; h++) {
      auto it = hashToIndex.find(h);
      if (it == hashToIndex.end()) continue;

      size_t fi = it->second;
      auto &f = s.Fields[fi];
      int foundBit = -1;
      auto bitIt = fieldBitMap.find(fi);
      if (bitIt != fieldBitMap.end())
        foundBit = bitIt->second;

      const CFieldCaptureInfo *ci = nullptr;
      if (fieldCapture && (*fieldCapture)[fi].kind != CFieldCaptureInfo::None)
        ci = &(*fieldCapture)[fi];

      out += std::format("        case {}:\n", h);
      out += std::format("          if (keyLen == {} && memcmp(key, \"{}\", {}) == 0) {{\n",
                         f.Name.size(), f.Name, f.Name.size());
      generateParseField(out, f, enumNames, foundBit, 6, opts.PascalCaseFields, ci);
      out += "          } else { return false; }\n";
      out += "          break;\n";
    }

    out += "        default:\n";
    out += "          return false;\n";
    out += "      }\n";
  }

  out += "      s.skipWhitespace();\n";
  out += "      if (s.p < s.end && *s.p == ',') { s.p++; continue; }\n";
  out += "      break;\n";
  out += "    }\n";
  out += "  }\n";
  out += "  if (!s.expectChar('}')) return false;\n";

  if (requiredCount > 0) {
    uint64_t mask = ((uint64_t)1 << requiredCount) - 1;
    out += std::format("  return valid && (found & 0x{:x}) == 0x{:x};\n", mask, mask);
  } else {
    out += "  return valid;\n";
  }
}

// ============================================================================
// Code generation — declarations (header)
// ============================================================================

static void generateCaptureFields(std::string &out, const CStructDef &s,
    const CContextAnalysis &ca,
    const CCodegenOptions &opts)
{
  for (size_t i = 0; i < s.Fields.size(); i++) {
    auto &ci = ca.FieldCapture[i];
    auto &f = s.Fields[i];
    if (ci.kind == CFieldCaptureInfo::MappedDirect) {
      out += std::format("    std::string {};\n", ci.CaptureFieldName);
    } else if (ci.kind == CFieldCaptureInfo::NestedStruct) {
      std::string childCapture = opts.StructPrefix + f.Type.RefName + "::Capture";
      bool isArray = (f.Kind == EFieldKind::Array || f.Kind == EFieldKind::OptionalArray || f.Kind == EFieldKind::NullableArray);
      if (isArray)
        out += std::format("    std::vector<{}> {};\n", childCapture, ci.CaptureFieldName);
      else
        out += std::format("    {} {};\n", childCapture, ci.CaptureFieldName);
    }
  }
}

static std::string capitalizeFirst(const std::string &s) {
  if (s.empty()) return s;
  std::string r = s;
  r[0] = toupper(r[0]);
  return r;
}

static void generateStructDecl(std::string &out, const CStructDef &s,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts,
    const CContextAnalysis &ca)
{
  std::string sn = opts.StructPrefix + s.Name;

  out += std::format("struct {} {{\n", sn);

  // Capture struct (empty for non-capture structs)
  out += "  struct Capture {\n";
  if (ca.hasContext())
    generateCaptureFields(out, s, ca, opts);
  out += "  };\n";

  if (ca.hasContext()) {
    for (auto &n : ca.Needs)
      out += std::format("  using {}Context = {};\n", capitalizeFirst(n.MappedTypeName), n.CppContextType);
  }
  out += "\n";

  // Fields
  for (auto &f : s.Fields) {
    std::string type = cppFieldType(f, enumNames, opts.StructPrefix);
    std::string cn = fieldCppName(f.Name, opts.PascalCaseFields);
    std::string def = cppDefault(f, enumNames, opts.PascalCaseFields);
    if (!def.empty())
      out += std::format("  {} {} = {};\n", type, cn, def);
    else
      out += std::format("  {} {};\n", type, cn);
  }

  out += "\n";

  // Parse/serialize methods
  if (ca.hasContext()) {
    std::string ctxParams = ca.contextParams();
    out += "  bool parse(const char *buf, size_t bufSize, Capture &capture);\n";
    out += "  bool parseImpl(JsonScanner &s, Capture *capture);\n";
    out += std::format("  static bool resolve({} &out, const Capture &capture, {});\n", sn, ctxParams);
    out += std::format("  void serialize(xmstream &out, {}) const;\n", ctxParams);
  } else {
    out += "  bool parse(const char *buf, size_t bufSize);\n";
    out += "  bool parseImpl(JsonScanner &s);\n";
    out += "  void serialize(xmstream &out) const;\n";
    if (opts.VerboseMode) {
      out += "  bool parseVerbose(const char *buf, size_t bufSize, ParseError &error);\n";
      out += "  bool parseVerboseImpl(VerboseJsonScanner &s);\n";
    }
  }

  // Tagged schema
  if (s.HasTaggedSchema) {
    out += "\n  static constexpr auto schema() {\n";
    out += "    return std::make_tuple(\n";
    bool first = true;
    for (auto &f : s.Fields) {
      if (!first) out += ",\n";
      first = false;
      std::string cn = fieldCppName(f.Name, opts.PascalCaseFields);
      out += std::format("      field<{}, &{}::{}>()", f.Tag, sn, cn);
    }
    out += "\n    );\n";
    out += "  }\n";
  }

  // Embedded C++ code
  for (auto &code : s.CppBlocks) {
    out += "\n";
    // Indent each line by 2 spaces
    size_t pos = 0;
    while (pos < code.size()) {
      size_t eol = code.find('\n', pos);
      if (eol == std::string::npos) eol = code.size();
      out += "  ";
      out.append(code, pos, eol - pos);
      out += "\n";
      pos = eol + 1;
    }
  }

  out += "};\n\n";
}

// ============================================================================
// Code generation — implementations (source)
// ============================================================================


static void generateResolveImpl(std::string &out, const CStructDef &s,
    const CContextAnalysis &ca,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts)
{
  std::string sn = opts.StructPrefix + s.Name;
  std::string ctxArgs = ca.contextArgs();

  if (ca.hasContext())
    out += std::format("bool {}::resolve({} &out, const Capture &capture, {}) {{\n",
                       sn, sn, ca.contextParams());
  else
    out += std::format("bool {}::resolve({} &out, const Capture &capture) {{\n", sn, sn);

  for (size_t i = 0; i < s.Fields.size(); i++) {
    auto &ci = ca.FieldCapture[i];
    auto &f = s.Fields[i];
    std::string cn = fieldCppName(f.Name, opts.PascalCaseFields);

    if (ci.kind == CFieldCaptureInfo::MappedDirect) {
      std::string resolveFunc = "__" + ci.MappedTypeName + "Resolve";
      out += std::format("  if (!{}(capture.{}, {}, out.{})) return false;\n",
                         resolveFunc, ci.CaptureFieldName, ctxArgs, cn);
    } else if (ci.kind == CFieldCaptureInfo::NestedStruct) {
      std::string childStruct = opts.StructPrefix + f.Type.RefName;
      std::string cce0 = childCtxExpr(ci, "");
      std::string cceI = childCtxExpr(ci, "i");

      switch (f.Kind) {
        case EFieldKind::Required:
        case EFieldKind::Optional:
          out += std::format("  if (!{}::resolve(out.{}, capture.{}, {})) return false;\n",
                             childStruct, cn, ci.CaptureFieldName, cce0);
          break;
        case EFieldKind::OptionalObject:
        case EFieldKind::NullableObject:
          out += std::format("  if (out.{}.has_value()) {{\n", cn);
          out += std::format("    if (!{}::resolve(*out.{}, capture.{}, {})) return false;\n",
                             childStruct, cn, ci.CaptureFieldName, cce0);
          out += "  }\n";
          break;
        case EFieldKind::Array:
          out += std::format("  for (size_t i = 0; i < out.{}.size(); i++) {{\n", cn);
          out += std::format("    if (!{}::resolve(out.{}[i], capture.{}[i], {})) return false;\n",
                             childStruct, cn, ci.CaptureFieldName, cceI);
          out += "  }\n";
          break;
        case EFieldKind::OptionalArray:
        case EFieldKind::NullableArray:
          out += std::format("  if (out.{}.has_value()) {{\n", cn);
          out += std::format("    for (size_t i = 0; i < out.{}->size(); i++) {{\n", cn);
          out += std::format("      if (!{}::resolve((*out.{})[i], capture.{}[i], {})) return false;\n",
                             childStruct, cn, ci.CaptureFieldName, cceI);
          out += "    }\n";
          out += "  }\n";
          break;
      }
    }
  }

  out += "  return true;\n";
  out += "}\n\n";
}

// Serialize a context-free mapped field using its format function
static void emitMappedInlineSerialize(std::string &out, const CFieldDef &f,
    const CFieldCaptureInfo &fci, int ind, bool &first, bool pascalCase)
{
  std::string in = indent(ind);
  std::string cn = fieldCppName(f.Name, pascalCase);
  auto wi = getWireTypeInfo(fci.MappedWireType);
  std::string formatFunc = "__" + fci.MappedTypeName + "Format";

  auto emitKey = [&](const std::string &jsonKey) {
    if (!first)
      out += std::format("{}out.write(',');\n", in);
    first = false;
    out += std::format("{}out.write(\"\\\"{}\\\":\");\n", in, jsonKey);
  };

  switch (f.Kind) {
    case EFieldKind::Required:
    case EFieldKind::Optional:
      emitKey(f.Name);
      out += std::format("{}{}(out, {}({}));\n", in, wi.writeFunc, formatFunc, cn);
      break;
    case EFieldKind::OptionalObject:
      out += std::format("{}if ({}.has_value()) {{\n", in, cn);
      if (!first)
        out += std::format("{}  out.write(',');\n", in);
      out += std::format("{}  out.write(\"\\\"{}\\\":\");\n", in, f.Name);
      out += std::format("{}  {}(out, {}(*{}));\n", in, wi.writeFunc, formatFunc, cn);
      first = false;
      out += std::format("{}}}\n", in);
      break;
    case EFieldKind::NullableObject:
      emitKey(f.Name);
      out += std::format("{}if ({}.has_value()) {{\n", in, cn);
      out += std::format("{}  {}(out, {}(*{}));\n", in, wi.writeFunc, formatFunc, cn);
      out += std::format("{}}} else {{\n", in);
      out += std::format("{}  out.write(\"null\");\n", in);
      out += std::format("{}}}\n", in);
      break;
    case EFieldKind::Array:
      emitKey(f.Name);
      out += std::format("{}out.write('[');\n", in);
      out += std::format("{}for (size_t i_ = 0; i_ < {}.size(); i_++) {{\n", in, cn);
      out += std::format("{}  if (i_) out.write(',');\n", in);
      out += std::format("{}  {}(out, {}({}[i_]));\n", in, wi.writeFunc, formatFunc, cn);
      out += std::format("{}}}\n", in);
      out += std::format("{}out.write(']');\n", in);
      break;
    case EFieldKind::OptionalArray:
      out += std::format("{}if ({}.has_value()) {{\n", in, cn);
      if (!first)
        out += std::format("{}  out.write(',');\n", in);
      out += std::format("{}  out.write(\"\\\"{}\\\":\");\n", in, f.Name);
      out += std::format("{}  out.write('[');\n", in);
      out += std::format("{}  for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", in, cn);
      out += std::format("{}    if (i_) out.write(',');\n", in);
      out += std::format("{}    {}(out, {}((*{})[i_]));\n", in, wi.writeFunc, formatFunc, cn);
      out += std::format("{}  }}\n", in);
      out += std::format("{}  out.write(']');\n", in);
      first = false;
      out += std::format("{}}}\n", in);
      break;
    case EFieldKind::NullableArray:
      emitKey(f.Name);
      out += std::format("{}if ({}.has_value()) {{\n", in, cn);
      out += std::format("{}  out.write('[');\n", in);
      out += std::format("{}  for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", in, cn);
      out += std::format("{}    if (i_) out.write(',');\n", in);
      out += std::format("{}    {}(out, {}((*{})[i_]));\n", in, wi.writeFunc, formatFunc, cn);
      out += std::format("{}  }}\n", in);
      out += std::format("{}  out.write(']');\n", in);
      out += std::format("{}}} else {{\n", in);
      out += std::format("{}  out.write(\"null\");\n", in);
      out += std::format("{}}}\n", in);
      break;
  }
}

// Generate the context-aware serialize body
static void generateContextSerializeBody(std::string &out, const CStructDef &s,
    const CContextAnalysis &ca,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts)
{
  std::string ctxArgs = ca.contextArgs();
  bool first = true;

  for (size_t i = 0; i < s.Fields.size(); i++) {
    auto &ci = ca.FieldCapture[i];
    auto &f = s.Fields[i];
    std::string in = indent(1);
    std::string cn = fieldCppName(f.Name, opts.PascalCaseFields);

    auto emitKey = [&](const std::string &jsonKey) {
      if (!first)
        out += std::format("{}out.write(',');\n", in);
      first = false;
      out += std::format("{}out.write(\"\\\"{}\\\":\");\n", in, jsonKey);
    };

    if (ci.kind == CFieldCaptureInfo::MappedDirect) {
      std::string formatFunc = "__" + ci.MappedTypeName + "Format";
      emitKey(f.Name);
      out += std::format("{}jsonWriteString(out, {}({}, {}));\n", in, formatFunc, cn, ctxArgs);
      continue;
    }

    if (ci.kind == CFieldCaptureInfo::NestedStruct) {
      std::string cce0 = childCtxExpr(ci, "");
      std::string cceI = childCtxExpr(ci, "i_");
      switch (f.Kind) {
        case EFieldKind::Required:
        case EFieldKind::Optional:
          emitKey(f.Name);
          out += std::format("{}{}.serialize(out, {});\n", in, cn, cce0);
          break;
        case EFieldKind::OptionalObject: {
          out += std::format("{}if ({}.has_value()) {{\n", in, cn);
          std::string in2 = indent(2);
          if (!first)
            out += std::format("{}out.write(',');\n", in2);
          out += std::format("{}out.write(\"\\\"{}\\\":\");\n", in2, f.Name);
          out += std::format("{}{}->serialize(out, {});\n", in2, cn, cce0);
          first = false;
          out += std::format("{}}}\n", in);
          break;
        }
        case EFieldKind::NullableObject: {
          emitKey(f.Name);
          out += std::format("{}if ({}.has_value()) {{\n", in, cn);
          out += std::format("{}  {}->serialize(out, {});\n", in, cn, cce0);
          out += std::format("{}}} else {{\n", in);
          out += std::format("{}  out.write(\"null\");\n", in);
          out += std::format("{}}}\n", in);
          break;
        }
        case EFieldKind::Array:
          emitKey(f.Name);
          out += std::format("{}out.write('[');\n", in);
          out += std::format("{}for (size_t i_ = 0; i_ < {}.size(); i_++) {{\n", in, cn);
          out += std::format("{}  if (i_) out.write(',');\n", in);
          out += std::format("{}  {}[i_].serialize(out, {});\n", in, cn, cceI);
          out += std::format("{}}}\n", in);
          out += std::format("{}out.write(']');\n", in);
          break;
        case EFieldKind::OptionalArray: {
          out += std::format("{}if ({}.has_value()) {{\n", in, cn);
          std::string in2 = indent(2);
          if (!first)
            out += std::format("{}out.write(',');\n", in2);
          out += std::format("{}out.write(\"\\\"{}\\\":\");\n", in2, f.Name);
          out += std::format("{}out.write('[');\n", in2);
          out += std::format("{}for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", in2, cn);
          out += std::format("{}  if (i_) out.write(',');\n", in2);
          out += std::format("{}  (*{})[i_].serialize(out, {});\n", in2, cn, cceI);
          out += std::format("{}}}\n", in2);
          out += std::format("{}out.write(']');\n", in2);
          first = false;
          out += std::format("{}}}\n", in);
          break;
        }
        case EFieldKind::NullableArray: {
          emitKey(f.Name);
          out += std::format("{}if ({}.has_value()) {{\n", in, cn);
          std::string in2 = indent(2);
          out += std::format("{}out.write('[');\n", in2);
          out += std::format("{}for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", in2, cn);
          out += std::format("{}  if (i_) out.write(',');\n", in2);
          out += std::format("{}  (*{})[i_].serialize(out, {});\n", in2, cn, cceI);
          out += std::format("{}}}\n", in2);
          out += std::format("{}out.write(']');\n", in2);
          out += std::format("{}}} else {{\n", in);
          out += std::format("{}  out.write(\"null\");\n", in);
          out += std::format("{}}}\n", in);
          break;
        }
      }
      continue;
    }

    // Regular field or context-free mapped field
    if (ci.kind == CFieldCaptureInfo::MappedInline)
      emitMappedInlineSerialize(out, f, ci, 1, first, opts.PascalCaseFields);
    else
      generateSerializeField(out, f, enumNames, 1, first, opts.PascalCaseFields);
  }
}

static void generateStructImpl(std::string &out, const CStructDef &s,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts,
    const CContextAnalysis &ca)
{
  std::string sn = opts.StructPrefix + s.Name;
  bool hasCtx = ca.hasContext();

  // Compute perfect hash
  std::vector<std::string> fieldNames;
  for (auto &f : s.Fields)
    fieldNames.push_back(f.Name);

  auto ph = findPerfectHash(fieldNames);
  if (!ph) {
    fprintf(stderr, "warning: cannot find perfect hash for struct '%s', using mod=%zu\n",
            s.Name.c_str(), fieldNames.size() * 4);
    ph = CPerfectHash{0, 31, (uint32_t)(fieldNames.size() * 4)};
  }

  // Count required fields
  auto isRequired = [](EFieldKind k) {
    return k == EFieldKind::Required || k == EFieldKind::Array;
  };
  int requiredCount = 0;
  for (auto &f : s.Fields)
    if (isRequired(f.Kind))
      requiredCount++;

  // Build hash -> field index map
  std::unordered_map<uint32_t, size_t> hashToIndex;
  for (size_t i = 0; i < s.Fields.size(); i++) {
    uint32_t h = computeHash(s.Fields[i].Name.c_str(), s.Fields[i].Name.size(),
                              ph->Seed, ph->Mult, ph->Mod);
    hashToIndex[h] = i;
  }

  // Assign required field bits
  std::unordered_map<size_t, int> fieldBitMap;
  int bitIndex = 0;
  for (size_t i = 0; i < s.Fields.size(); i++)
    if (isRequired(s.Fields[i].Kind))
      fieldBitMap[i] = bitIndex++;

  if (hasCtx) {
    // --- parse(buf, bufSize, capture) ---
    out += std::format("bool {}::parse(const char *buf, size_t bufSize, Capture &capture) {{\n", sn);
    out += "  JsonScanner s{buf, buf + bufSize};\n";
    out += "  return parseImpl(s, &capture);\n";
    out += "}\n\n";

    // --- parseImpl(s, capture) ---
    out += std::format("bool {}::parseImpl(JsonScanner &s, Capture *capture) {{\n", sn);
    generateParseBody(out, s, enumNames, opts, *ph, hashToIndex, fieldBitMap, requiredCount, &ca.FieldCapture);
    out += "}\n\n";
  } else {
    // --- parse(buf, bufSize) ---
    out += std::format("bool {}::parse(const char *buf, size_t bufSize) {{\n", sn);
    out += "  JsonScanner s{buf, buf + bufSize};\n";
    out += "  return parseImpl(s);\n";
    out += "}\n\n";

    // --- parseImpl(s) ---
    out += std::format("bool {}::parseImpl(JsonScanner &s) {{\n", sn);
    generateParseBody(out, s, enumNames, opts, *ph, hashToIndex, fieldBitMap, requiredCount);
    out += "}\n\n";
  }

  // --- Verbose parse ---
  if (opts.VerboseMode && !hasCtx) {
    out += std::format("bool {}::parseVerbose(const char *buf, size_t bufSize, ParseError &error) {{\n", sn);
    out += "  VerboseJsonScanner s{buf, buf + bufSize, buf, &error};\n";
    out += "  return parseVerboseImpl(s);\n";
    out += "}\n\n";

    out += std::format("bool {}::parseVerboseImpl(VerboseJsonScanner &s) {{\n", sn);
    out += "  if (!s.expectChar('{')) return false;\n";
    if (requiredCount > 0)
      out += "  uint64_t found = 0;\n";
    out += "  s.skipWhitespace();\n";
    out += "  if (s.p < s.end && *s.p != '}') {\n";
    out += "    for (;;) {\n";
    out += "      const char *key;\n";
    out += "      size_t keyLen;\n";

    if (s.Fields.empty()) {
      out += "      if (!s.readString(key, keyLen)) return false;\n";
      out += "      if (!s.expectChar(':')) return false;\n";
      out += "      s.setError(\"unknown field '\" + std::string(key, keyLen) + \"'\");\n";
      out += "      return false;\n";
    } else {
      out += "      uint32_t keyHash;\n";
      out += std::format("      if (!s.readStringHash(key, keyLen, keyHash, {}, {})) return false;\n",
                         ph->Seed, ph->Mult);
      out += "      if (!s.expectChar(':')) return false;\n";
      out += std::format("      switch (keyHash % {}) {{\n", ph->Mod);

      for (uint32_t h = 0; h < ph->Mod; h++) {
        auto it = hashToIndex.find(h);
        if (it == hashToIndex.end()) continue;

        size_t fi = it->second;
        auto &f = s.Fields[fi];
        int foundBit = -1;
        auto bitIt = fieldBitMap.find(fi);
        if (bitIt != fieldBitMap.end())
          foundBit = bitIt->second;

        out += std::format("        case {}:\n", h);
        out += std::format("          if (keyLen == {} && memcmp(key, \"{}\", {}) == 0) {{\n",
                           f.Name.size(), f.Name, f.Name.size());

        std::string verboseFieldCode;
        generateVerboseParseField(verboseFieldCode, f, enumNames, foundBit, 6, opts.PascalCaseFields);
        out += verboseFieldCode;

        out += "          } else { s.setError(\"unknown field '\" + std::string(key, keyLen) + \"'\"); return false; }\n";
        out += "          break;\n";
      }

      out += "        default:\n";
      out += "          s.setError(\"unknown field '\" + std::string(key, keyLen) + \"'\");\n";
      out += "          return false;\n";
      out += "      }\n";
    }

    out += "      s.skipWhitespace();\n";
    out += "      if (s.p < s.end && *s.p == ',') { s.p++; continue; }\n";
    out += "      break;\n";
    out += "    }\n";
    out += "  }\n";
    out += "  if (!s.expectChar('}')) return false;\n";

    if (requiredCount > 0) {
      uint64_t mask = ((uint64_t)1 << requiredCount) - 1;
      out += std::format("  if ((found & 0x{:x}) != 0x{:x}) {{\n", mask, mask);
      for (size_t i = 0; i < s.Fields.size(); i++) {
        auto bitIt2 = fieldBitMap.find(i);
        if (bitIt2 != fieldBitMap.end()) {
          out += std::format("    if (!(found & ((uint64_t)1 << {}))) "
                             "{{ s.setError(\"missing required field '{}'\"); return false; }}\n",
                             bitIt2->second, s.Fields[i].Name);
        }
      }
      out += "  }\n";
    }

    out += "  return true;\n";
    out += "}\n\n";
  }

  // --- resolve() ---
  if (hasCtx)
    generateResolveImpl(out, s, ca, enumNames, opts);

  if (hasCtx) {
    // Context-aware serialize (with or without external params)
    if (hasCtx) {
      std::string ctxParams = ca.contextParams();
      out += std::format("void {}::serialize(xmstream &out, {}) const {{\n", sn, ctxParams);
    } else {
      // Self-contained: no external context params
      out += std::format("void {}::serialize(xmstream &out) const {{\n", sn);
    }
    out += "  out.write('{');\n";
    generateContextSerializeBody(out, s, ca, enumNames, opts);
    out += "  out.write('}');\n";
    out += "}\n\n";
  } else {
    // --- serialize() ---
    out += std::format("void {}::serialize(xmstream &out) const {{\n", sn);
    out += "  out.write('{');\n";
    {
      bool first = true;
      for (size_t i = 0; i < s.Fields.size(); i++) {
        auto &fci = ca.FieldCapture[i];
        if (fci.kind == CFieldCaptureInfo::MappedInline) {
          emitMappedInlineSerialize(out, s.Fields[i], fci, 1, first, opts.PascalCaseFields);
        } else {
          generateSerializeField(out, s.Fields[i], enumNames, 1, first, opts.PascalCaseFields);
        }
      }
    }
    out += "  out.write('}');\n";
    out += "}\n\n";
  }
}

// ============================================================================
// Main entry point
// ============================================================================

CCodegenResult generateCode(const CIdlFile &file, const std::string &headerName, const CCodegenOptions &opts)
{
  CCodegenResult result;
  auto &header = result.Header;
  auto &source = result.Source;

  // Collect enum names
  std::unordered_set<std::string> enumNames;
  for (auto &e : file.Enums)
    enumNames.insert(e.Name);

  // Build mapped type index
  std::unordered_map<std::string, const CMappedTypeDef *> mappedTypeIndex;
  for (auto &m : file.MappedTypes)
    mappedTypeIndex[m.Name] = &m;

  // Analyze context for all structs (topological order — children first)
  std::unordered_map<std::string, CContextAnalysis> contextAnalysis;
  for (auto &s : file.Structs) {
    if (s.IsMixin) continue;
    contextAnalysis[s.Name] = analyzeContext(s, opts.PascalCaseFields, mappedTypeIndex, contextAnalysis);
  }

  // Check features
  bool hasTaggedSchema = false;
  bool hasChrono = false;
  for (auto &s : file.Structs) {
    if (!s.IsMixin && !s.IsImported) {
      if (s.HasTaggedSchema) hasTaggedSchema = true;
      for (auto &f : s.Fields) {
        if (f.Type.IsScalar && (f.Type.Scalar == EScalarType::Seconds ||
            f.Type.Scalar == EScalarType::Minutes || f.Type.Scalar == EScalarType::Hours))
          hasChrono = true;
      }
    }
  }

  // === Header ===
  header += "// Generated by idltool — do not edit\n";
  header += "#pragma once\n\n";
  header += "#include <string>\n";
  header += "#include <vector>\n";
  header += "#include <optional>\n";
  header += "#include <cstdint>\n";
  header += "#include <ctime>\n";
  if (hasChrono)
    header += "#include <chrono>\n";
  if (hasTaggedSchema)
    header += "#include \"idltool/schema.h\"\n";

  // Mapped type headers
  for (auto &m : file.MappedTypes) {
    if (!m.IsImported)
      header += std::format("#include \"{}\"\n", m.IncludePath);
  }
  header += "\n";

  // Include directives
  for (auto &inc : file.Includes)
    header += std::format("#include \"{}.h\"\n", inc.Path);
  if (!file.Includes.empty())
    header += "\n";

  // Forward declarations
  header += "class xmstream;\n";
  header += "struct JsonScanner;\n";
  if (opts.VerboseMode) {
    header += parseErrorCode;
    header += "struct VerboseJsonScanner;\n";
  }
  header += "\n";

  // Enums
  generateEnumDeclarations(header, file, opts.PascalCaseFields);

  // Forward declarations for structs
  for (auto &s : file.Structs) {
    if (s.IsMixin || s.IsImported) continue;
    header += std::format("struct {};\n", opts.StructPrefix + s.Name);
  }
  header += "\n";

  // Struct declarations (Capture is nested inside)
  for (auto &s : file.Structs) {
    if (s.IsMixin || s.IsImported) continue;
    auto &ca = contextAnalysis[s.Name];
    generateStructDecl(header, s, enumNames, opts, ca);
  }

  // === Source ===
  source += "// Generated by idltool — do not edit\n";
  source += std::format("#include \"{}\"\n", headerName);
  source += "#include \"p2putils/xmstream.h\"\n";
  source += "#include <cstring>\n";
  source += "#include <cstdlib>\n";
  source += "#include <cinttypes>\n\n";

  if (opts.Standalone)
    source += jsonScannerCode;
  if (opts.VerboseMode)
    source += verboseJsonScannerCode;
  source += jsonHelperCode;
  source += "\n";

  generateEnumDefinitions(source, file, opts.PascalCaseFields);

  for (auto &s : file.Structs) {
    if (s.IsMixin || s.IsImported) continue;
    generateStructImpl(source, s, enumNames, opts, contextAnalysis[s.Name]);
  }

  return result;
}
