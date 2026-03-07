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
  std::string MappedWireType;    // JSON wire type (e.g. "int64", "string")
  std::string DirectCtxParamName; // for MappedDirect: external param name (e.g. "moneyCtx")
  struct ChildContextArg {
    std::string MappedTypeName;
    size_t SlotOrdinal = 0;
    std::string ParamName;
    bool IsVector = false;
  };
  std::vector<ChildContextArg> ChildCtxArgs; // for NestedStruct
};

// Per-struct context analysis result
struct CContextAnalysis {
  struct ContextNeed {
    std::string MappedTypeName;   // IDL name, e.g. "money"
    std::string CppContextType;   // e.g. "uint32_t"
    size_t SlotOrdinal = 0;       // stable slot index among same-type contexts in this struct
    std::string ParamName;        // e.g. "moneyCtx"
    bool IsVector = false;        // true = pass as const std::vector<T>&
  };
  std::vector<ContextNeed> Needs;

  // Parallel to s.Fields
  std::vector<CFieldCaptureInfo> FieldCapture;

  bool hasContext() const { return !Needs.empty(); }
  bool hasVectorContext() const {
    for (auto &n : Needs) {
      if (n.IsVector)
        return true;
    }
    return false;
  }

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

  std::string broadcastContextParams() const {
    std::string result;
    for (auto &n : Needs) {
      if (!result.empty()) result += ", ";
      result += n.CppContextType + " " + n.ParamName;
    }
    return result;
  }

  CContextAnalysis broadcastVariant() const {
    CContextAnalysis result = *this;
    for (auto &need : result.Needs)
      need.IsVector = false;
    for (auto &fieldCapture : result.FieldCapture) {
      for (auto &arg : fieldCapture.ChildCtxArgs)
        arg.IsVector = false;
    }
    return result;
  }
};

static bool isArrayLikeField(const CFieldDef &f)
{
  switch (f.Kind) {
    case EFieldKind::Array:
    case EFieldKind::OptionalArray:
    case EFieldKind::FixedArray:
    case EFieldKind::OptionalFixedArray:
      return true;
    default:
      return false;
  }
}

static bool isConditionallySerialized(const CFieldDef &f)
{
  if (f.Kind == EFieldKind::Optional)
    return true;
  return isOptionalField(f) && !fieldEmptyOutIsNull(f);
}

static std::string contextParamName(const std::string &mappedTypeName, size_t ordinal, size_t totalCount)
{
  if (totalCount <= 1)
    return mappedTypeName + "Ctx";
  return std::format("{}Ctx{}", mappedTypeName, ordinal + 1);
}

static std::string contextSlotKey(const std::string &mappedTypeName, size_t slotOrdinal)
{
  return std::format("{}#{}", mappedTypeName, slotOrdinal);
}

static CContextAnalysis analyzeContext(const CStructDef &s, bool pascalCase,
    const std::unordered_map<std::string, const CMappedTypeDef *> &mappedTypes,
    const std::unordered_map<std::string, CContextAnalysis> &childAnalysis)
{
  CContextAnalysis a;
  a.FieldCapture.resize(s.Fields.size());

  std::vector<bool> fieldNeedsContext(s.Fields.size(), false);
  for (size_t i = 0; i < s.Fields.size(); i++) {
    auto &f = s.Fields[i];
    if (f.Type.IsMapped) {
      auto it = mappedTypes.find(f.Type.RefName);
      if (it != mappedTypes.end() && it->second->ContextType)
        fieldNeedsContext[i] = true;
      continue;
    }

    if (!f.Type.RefName.empty() && !f.Type.IsScalar) {
      auto cit = childAnalysis.find(f.Type.RefName);
      if (cit != childAnalysis.end() && cit->second.hasContext())
        fieldNeedsContext[i] = true;
    }
  }

  std::unordered_map<std::string, size_t> fieldToGroup;
  for (size_t groupIndex = 0; groupIndex < s.ContextGroups.size(); groupIndex++) {
    for (auto &fieldName : s.ContextGroups[groupIndex].FieldNames)
      fieldToGroup[fieldName] = groupIndex;
  }

  size_t groupCount = s.ContextGroups.size();
  for (size_t i = 0; i < s.Fields.size(); i++) {
    if (fieldNeedsContext[i] && !fieldToGroup.contains(s.Fields[i].Name))
      fieldToGroup[s.Fields[i].Name] = groupCount++;
  }

  struct CGroupNeed {
    std::string MappedTypeName;
    std::string CppContextType;
    size_t SourceSlotOrdinal = 0;
    bool HasScalarUse = false;
    bool HasVectorUse = false;
  };
  std::vector<std::vector<CGroupNeed>> groupNeeds(groupCount);

  auto mergeGroupNeed = [&](size_t groupIndex, const std::string &mappedTypeName,
                            const std::string &cppContextType, size_t sourceSlotOrdinal, bool isVector) {
    for (auto &need : groupNeeds[groupIndex]) {
      if (need.MappedTypeName == mappedTypeName && need.SourceSlotOrdinal == sourceSlotOrdinal) {
        if (isVector)
          need.HasVectorUse = true;
        else
          need.HasScalarUse = true;
        return;
      }
    }
    CGroupNeed need;
    need.MappedTypeName = mappedTypeName;
    need.CppContextType = cppContextType;
    need.SourceSlotOrdinal = sourceSlotOrdinal;
    if (isVector)
      need.HasVectorUse = true;
    else
      need.HasScalarUse = true;
    groupNeeds[groupIndex].push_back(std::move(need));
  };

  for (size_t i = 0; i < s.Fields.size(); i++) {
    auto &f = s.Fields[i];
    auto fieldGroupIt = fieldToGroup.find(f.Name);
    bool hasGroup = fieldGroupIt != fieldToGroup.end();

    if (f.Type.IsMapped) {
      auto it = mappedTypes.find(f.Type.RefName);
      if (it != mappedTypes.end() && it->second->ContextType) {
        a.FieldCapture[i].kind = CFieldCaptureInfo::MappedDirect;
        a.FieldCapture[i].CaptureFieldName = fieldCppName(f.Name, pascalCase);
        a.FieldCapture[i].MappedTypeName = f.Type.RefName;
        a.FieldCapture[i].MappedWireType = !f.Type.MappedWireType.empty() ? f.Type.MappedWireType : it->second->JsonWireType;
        if (hasGroup) {
          mergeGroupNeed(fieldGroupIt->second, f.Type.RefName, cppScalarType(*it->second->ContextType), 0, false);
        }
        continue;
      }

      if (it != mappedTypes.end()) {
        a.FieldCapture[i].kind = CFieldCaptureInfo::MappedInline;
        a.FieldCapture[i].CaptureFieldName = fieldCppName(f.Name, pascalCase);
        a.FieldCapture[i].MappedTypeName = f.Type.RefName;
        a.FieldCapture[i].MappedWireType = !f.Type.MappedWireType.empty() ? f.Type.MappedWireType : it->second->JsonWireType;
      }
      continue;
    }

    if (!f.Type.RefName.empty() && !f.Type.IsScalar) {
      auto cit = childAnalysis.find(f.Type.RefName);
      if (cit != childAnalysis.end() && cit->second.hasContext()) {
        a.FieldCapture[i].kind = CFieldCaptureInfo::NestedStruct;
        a.FieldCapture[i].CaptureFieldName = fieldCppName(f.Name, pascalCase);
        for (auto &need : cit->second.Needs) {
          bool needVector = need.IsVector || isArrayLikeField(f);
          a.FieldCapture[i].ChildCtxArgs.push_back({need.MappedTypeName, need.SlotOrdinal, "", needVector});
          if (hasGroup)
            mergeGroupNeed(fieldGroupIt->second, need.MappedTypeName, need.CppContextType, need.SlotOrdinal, needVector);
        }
      }
    }
  }

  std::unordered_map<std::string, size_t> perTypeGroupCount;
  for (auto &group : groupNeeds) {
    for (auto &need : group)
      perTypeGroupCount[need.MappedTypeName]++;
  }

  std::unordered_map<std::string, size_t> perTypeOrdinal;
  std::vector<std::unordered_map<std::string, std::string>> groupParamNames(groupNeeds.size());
  std::vector<std::unordered_map<std::string, bool>> groupParamVector(groupNeeds.size());
  for (size_t groupIndex = 0; groupIndex < groupNeeds.size(); groupIndex++) {
    for (auto &need : groupNeeds[groupIndex]) {
      size_t ordinal = perTypeOrdinal[need.MappedTypeName]++;
      std::string paramName = contextParamName(need.MappedTypeName, ordinal, perTypeGroupCount[need.MappedTypeName]);
      bool useVector = need.HasVectorUse && !need.HasScalarUse;
      std::string slotKey = contextSlotKey(need.MappedTypeName, need.SourceSlotOrdinal);
      groupParamNames[groupIndex][slotKey] = paramName;
      groupParamVector[groupIndex][slotKey] = useVector;
      a.Needs.push_back({need.MappedTypeName, need.CppContextType, ordinal, paramName, useVector});
    }
  }

  for (size_t i = 0; i < s.Fields.size(); i++) {
    auto groupIt = fieldToGroup.find(s.Fields[i].Name);
    if (groupIt == fieldToGroup.end())
      continue;
    auto &capture = a.FieldCapture[i];
    if (capture.kind == CFieldCaptureInfo::MappedDirect) {
      capture.DirectCtxParamName = groupParamNames[groupIt->second][contextSlotKey(capture.MappedTypeName, 0)];
    } else if (capture.kind == CFieldCaptureInfo::NestedStruct) {
      for (auto &arg : capture.ChildCtxArgs) {
        std::string slotKey = contextSlotKey(arg.MappedTypeName, arg.SlotOrdinal);
        arg.ParamName = groupParamNames[groupIt->second][slotKey];
        arg.IsVector = groupParamVector[groupIt->second][slotKey];
      }
    }
  }

  return a;
}

static std::vector<CArrayDim> arrayDims(const CFieldDef &f)
{
  std::vector<CArrayDim> dims;
  switch (f.Kind) {
    case EFieldKind::Array:
    case EFieldKind::OptionalArray:
      dims.push_back({});
      break;
    case EFieldKind::FixedArray:
    case EFieldKind::OptionalFixedArray:
      dims.push_back({f.Type.FixedSize});
      break;
    default:
      break;
  }
  dims.insert(dims.end(), f.Type.InnerDims.begin(), f.Type.InnerDims.end());
  return dims;
}

static std::string wrapDimsType(std::string baseType, const std::vector<CArrayDim> &dims)
{
  for (int i = (int)dims.size() - 1; i >= 0; i--) {
    if (dims[i].FixedSize > 0)
      baseType = std::format("std::array<{}, {}>", baseType, dims[i].FixedSize);
    else
      baseType = std::format("std::vector<{}>", baseType);
  }
  return baseType;
}

static std::string captureFieldType(const CFieldDef &f, const std::string &baseType)
{
  return wrapDimsType(baseType, arrayDims(f));
}

static bool hasVectorChildContext(const CFieldCaptureInfo &ci)
{
  for (auto &arg : ci.ChildCtxArgs) {
    if (arg.IsVector)
      return true;
  }
  return false;
}

// Generate the expression list to pass context to a nested child
static std::string childCtxArgsExpr(const CFieldCaptureInfo &ci,
                                    const std::string &indexExpr)
{
  std::string result;
  for (auto &arg : ci.ChildCtxArgs) {
    if (!result.empty())
      result += ", ";
    if (arg.IsVector && !indexExpr.empty())
      result += arg.ParamName + "[" + indexExpr + "]";
    else
      result += arg.ParamName;
  }
  return result;
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
  if (wireType == "uint64") return {"readUInt64", "jsonWriteUInt", "uint64_t"};
  if (wireType == "int32")  return {"readInt32", "jsonWriteInt", "int32_t"};
  if (wireType == "uint32") return {"readUInt32", "jsonWriteUInt", "uint32_t"};
  if (wireType == "double") return {"readDouble", "jsonWriteDouble", "double"};
  if (wireType == "bool")   return {"readBool", "jsonWriteBool", "bool"};
  return {"readStringValue", "jsonWriteString", "std::string"};
}

static void generateDirectMappedCaptureRead(std::string &out,
                                            const CFieldCaptureInfo &ci,
                                            const std::string &captureExpr,
                                            int ind,
                                            const std::string &failureCode)
{
  auto wi = getWireTypeInfo(ci.MappedWireType);
  std::string in = indent(ind);
  out += std::format("{}if (capture) {{\n", in);
  out += std::format("{}  if (!s.{}({})) {{ {}; }}\n", in, wi.readMethod, captureExpr, failureCode);
  out += std::format("{}}} else {{\n", in);
  out += std::format("{}  if (!s.skipValue()) {{ {}; }}\n", in, failureCode);
  out += std::format("{}}}\n", in);
}

static void generateParseMappedValue(std::string &out,
                                     const std::string &mappedTypeName,
                                     const std::string &mappedWireType,
                                     const std::string &destExpr,
                                     int ind,
                                     const std::string &failureCode)
{
  auto wi = getWireTypeInfo(mappedWireType);
  std::string in = indent(ind);
  std::string resolveFunc = "__" + mappedTypeName + "Resolve";
  out += std::format("{}{{ {} _tmp; if (!s.{}(_tmp) || !{}(_tmp, {})) {{ {}; }} }}\n",
                     in, wi.cppType, wi.readMethod, resolveFunc, destExpr, failureCode);
}

static void emitMappedInlineValueSerialize(CSerializeCodeBuilder &out,
                                           const std::string &mappedTypeName,
                                           const std::string &mappedWireType,
                                           const std::string &valueExpr,
                                           int ind)
{
  auto wi = getWireTypeInfo(mappedWireType);
  std::string in = indent(ind);
  std::string formatFunc = "__" + mappedTypeName + "Format";
  out.appendRaw(std::format("{}{}(out, {}({}));\n", in, wi.writeFunc, formatFunc, valueExpr));
}

static void emitMappedInlineNestedArraySerialize(CSerializeCodeBuilder &out,
                                                 const CFieldCaptureInfo &fci,
                                                 const std::vector<CArrayDim> &dims,
                                                 int dimIndex,
                                                 const std::string &valueExpr,
                                                 int ind)
{
  std::string in = indent(ind);
  if (dimIndex >= (int)dims.size()) {
    emitMappedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, valueExpr, ind);
    return;
  }

  out.writeLiteral(ind, "[");
  std::string idx = std::format("i{}_", dimIndex);
  out.appendRaw(std::format("{}for (size_t {} = 0; {} < {}.size(); {}++) {{\n", in, idx, idx, valueExpr, idx));
  out.appendRaw(std::format("{}  if ({}) out.write(',');\n", in, idx));
  emitMappedInlineNestedArraySerialize(out, fci, dims, dimIndex + 1,
                                       std::format("{}[{}]", valueExpr, idx), ind + 1);
  out.appendRaw(std::format("{}}}\n", in));
  out.writeLiteral(ind, "]");
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
      auto wi = getWireTypeInfo(ci->MappedWireType);
      out += std::format("{}if (capture) {{\n", in);
      out += std::format("{}  if (!s.{}(capture->{})) valid = false;\n", in, wi.readMethod, ci->CaptureFieldName);
      if (foundBit >= 0)
        out += std::format("{}  else found |= (uint64_t)1 << {};\n", in, foundBit);
      out += std::format("{}}} else {{\n", in);
      out += std::format("{}  if (!s.skipValue()) valid = false;\n", in);
      if (foundBit >= 0)
        out += std::format("{}  else found |= (uint64_t)1 << {};\n", in, foundBit);
      out += std::format("{}}}\n", in);
    } else {
      std::string mappedTypeName = (ci && ci->kind == CFieldCaptureInfo::MappedInline) ? ci->MappedTypeName : f.Type.RefName;
      std::string mappedWireType = (ci && !ci->MappedWireType.empty()) ? ci->MappedWireType : f.Type.MappedWireType;
      auto wi = getWireTypeInfo(mappedWireType);
      std::string resolveFunc = "__" + mappedTypeName + "Resolve";
      out += std::format("{}{{ {} _tmp; if (!s.{}(_tmp) || !{}(_tmp, {})) valid = false;\n",
                         in, wi.cppType, wi.readMethod, resolveFunc, cn);
      if (foundBit >= 0)
        out += std::format("{}  else found |= (uint64_t)1 << {};\n", in, foundBit);
      out += std::format("{}}}\n", in);
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

// Generate nested array parse code for multi-dimensional arrays
static void generateNestedArrayParse(std::string &out, const CFieldDef &f,
    const std::unordered_set<std::string> &enumNames,
    const std::vector<CArrayDim> &dims, int dimIndex,
    const std::string &containerExpr,
    const std::string &captureExpr,
    int ind,
    const CFieldCaptureInfo *ci = nullptr)
{
  std::string in = indent(ind);

  if (dimIndex >= (int)dims.size()) {
    // Leaf element: parse scalar/struct/enum into containerExpr
    if (isMappedField(f)) {
      if (ci && ci->kind == CFieldCaptureInfo::MappedDirect) {
        generateDirectMappedCaptureRead(out, *ci, captureExpr, ind, "valid = false; break");
      } else {
        generateParseMappedValue(out, f.Type.RefName, f.Type.MappedWireType, containerExpr, ind, "valid = false; break");
      }
    } else if (isStructRef(f, enumNames)) {
      if (ci && ci->kind == CFieldCaptureInfo::NestedStruct) {
        out += std::format("{}if (!{}.parseImpl(s, capture ? &{} : nullptr)) {{ valid = false; break; }}\n",
                           in, containerExpr, captureExpr);
      } else {
        out += std::format("{}if (!{}.parseImpl(s)) {{ valid = false; break; }}\n", in, containerExpr);
      }
    } else if (isEnum(f.Type.RefName, enumNames)) {
      out += std::format("{}{{ const char *eStr; size_t eLen;\n", in);
      out += std::format("{}  if (!s.readString(eStr, eLen) || !parseE{}(eStr, eLen, {})) {{ valid = false; break; }} }}\n",
                         in, f.Type.RefName, containerExpr);
    } else if (f.Type.IsScalar && (f.Type.Scalar == EScalarType::Seconds ||
               f.Type.Scalar == EScalarType::Minutes || f.Type.Scalar == EScalarType::Hours)) {
      std::string chronoType = cppScalarType(f.Type.Scalar);
      out += std::format("{}{{ int64_t _t; if (!s.readInt64(_t)) {{ valid = false; break; }} {} = {}(_t); }}\n",
                         in, containerExpr, chronoType);
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
      out += std::format("{}if (!s.{}({})) {{ valid = false; break; }}\n", in, readMethod, containerExpr);
    }
    return;
  }

  auto &dim = dims[dimIndex];
  std::string idx = std::format("i{}_", dimIndex);

  if (dim.FixedSize > 0) {
    // Fixed inner dimension
    out += std::format("{}if (!s.expectChar('[')) {{ valid = false; break; }}\n", in);
    out += std::format("{}for (size_t {} = 0; {} < {}; {}++) {{\n", in, idx, idx, dim.FixedSize, idx);
    out += std::format("{}  if ({} > 0 && !s.expectChar(',')) {{ valid = false; break; }}\n", in, idx);
    generateNestedArrayParse(out, f, enumNames, dims, dimIndex + 1,
                             std::format("{}[{}]", containerExpr, idx),
                             std::format("{}[{}]", captureExpr, idx),
                             ind + 1, ci);
    out += std::format("{}}}\n", in);
    out += std::format("{}if (!s.expectChar(']')) {{ valid = false; break; }}\n", in);
  } else {
    // Dynamic inner dimension
    out += std::format("{}if (!s.expectChar('[')) {{ valid = false; break; }}\n", in);
    out += std::format("{}s.skipWhitespace();\n", in);
    out += std::format("{}if (s.p < s.end && *s.p != ']') {{\n", in);
    out += std::format("{}  for (;;) {{\n", in);
    if (ci && (ci->kind == CFieldCaptureInfo::MappedDirect || ci->kind == CFieldCaptureInfo::NestedStruct))
      out += std::format("{}    if (capture) {}.emplace_back();\n", in, captureExpr);
    out += std::format("{}    {}.emplace_back();\n", in, containerExpr);
    generateNestedArrayParse(out, f, enumNames, dims, dimIndex + 1,
                             std::format("{}.back()", containerExpr),
                             std::format("{}.back()", captureExpr),
                             ind + 2, ci);
    out += std::format("{}    s.skipWhitespace();\n", in);
    out += std::format("{}    if (s.p < s.end && *s.p == ',') {{ s.p++; continue; }}\n", in);
    out += std::format("{}    break;\n", in);
    out += std::format("{}  }}\n", in);
    out += std::format("{}}}\n", in);
    out += std::format("{}if (!s.expectChar(']')) {{ valid = false; break; }}\n", in);
  }
}

static void generateParseField(std::string &out, const CFieldDef &f,
    const std::unordered_set<std::string> &enumNames, int foundBit, int ind,
    bool pascalCase, const CFieldCaptureInfo *ci);

static void generateVariantParseAttempt(std::string &out, const CVariantAlt &alt,
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
  out += std::format("{}    bool valid = true;\n", in);
  out += std::format("{}    do {{\n", in);
  generateParseField(out, tmp, enumNames, -1, ind + 2, false, nullptr);
  out += std::format("{}    }} while (false);\n", in);
  out += std::format("{}    return valid;\n", in);
  out += std::format("{}  }})();\n", in);
  out += std::format("{}  if (altOk && __variantBoundary(s.p)) {{\n", in);
  out += std::format("{}    {}.emplace<{}>(std::move({}));\n", in, varName, altIndex, altVar);
  out += std::format("{}    matched = true;\n", in);
  out += std::format("{}  }} else {{\n", in);
  out += std::format("{}    s.p = saved;\n", in);
  out += std::format("{}  }}\n", in);
  out += std::format("{}}}\n", in);
}

// Generate parse code for a variant field using ordered backtracking.
static void generateVariantParse(std::string &out, const CFieldDef &f,
    const std::string &varName,
    const std::unordered_set<std::string> &enumNames, int ind)
{
  std::string in = indent(ind);
  out += std::format("{}auto __variantBoundary = [&](const char *p_) {{\n", in);
  out += std::format("{}  return p_ >= s.end || *p_ == ',' || *p_ == ']' || *p_ == '}}' ||\n", in);
  out += std::format("{}         *p_ == ' ' || *p_ == '\\t' || *p_ == '\\r' || *p_ == '\\n';\n", in);
  out += std::format("{}}};\n", in);
  out += std::format("{}bool matched = false;\n", in);
  for (size_t i = 0; i < f.Type.Alternatives.size(); i++)
    generateVariantParseAttempt(out, f.Type.Alternatives[i], i, varName, enumNames, ind);
  out += std::format("{}if (!matched) valid = false;\n", in);
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

    case EFieldKind::OptionalObject: {
      if (fieldAllowsNullInput(f))
        out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      else
        out += std::format("{}if (s.readNull()) {{ valid = false; }}\n", in);
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
      if (foundBit >= 0)
        out += std::format("{}if (valid) found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::Array: {
      if (!f.Type.InnerDims.empty()) {
        // Multi-dimensional array: parse outermost dynamic array, then nest
        out += std::format("{}if (!s.expectChar('[')) {{ valid = false; break; }}\n", in);
        out += std::format("{}s.skipWhitespace();\n", in);
        out += std::format("{}if (s.p < s.end && *s.p != ']') {{\n", in);
        out += std::format("{}  for (;;) {{\n", in);
        if (ci && (ci->kind == CFieldCaptureInfo::MappedDirect || ci->kind == CFieldCaptureInfo::NestedStruct))
          out += std::format("{}    if (capture) capture->{}.emplace_back();\n", in, ci->CaptureFieldName);
        out += std::format("{}    {}.emplace_back();\n", in, cn);
        generateNestedArrayParse(out, f, enumNames, f.Type.InnerDims, 0,
                                 std::format("{}.back()", cn),
                                 ci ? std::format("capture->{}.back()", ci->CaptureFieldName) : "",
                                 ind + 2, ci);
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

      out += std::format("{}if (!s.expectChar('[')) {{ valid = false; break; }}\n", in);
      out += std::format("{}s.skipWhitespace();\n", in);
      out += std::format("{}if (s.p < s.end && *s.p != ']') {{\n", in);
      out += std::format("{}  for (;;) {{\n", in);

      if (isMappedField(f)) {
        if (ci && ci->kind == CFieldCaptureInfo::MappedInline) {
          auto wi = getWireTypeInfo(ci->MappedWireType);
          std::string resolveFunc = "__" + ci->MappedTypeName + "Resolve";
          out += std::format("{}    {{ {} _tmp; {}.emplace_back(); if (!s.{}(_tmp) || !{}(_tmp, {}.back())) {{ valid = false; break; }} }}\n",
                             in, wi.cppType, cn, wi.readMethod, resolveFunc, cn);
        } else if (ci && ci->kind == CFieldCaptureInfo::MappedDirect) {
          out += std::format("{}    if (capture) capture->{}.emplace_back();\n", in, ci->CaptureFieldName);
          out += std::format("{}    {}.emplace_back();\n", in, cn);
          generateDirectMappedCaptureRead(out, *ci, std::format("capture->{}.back()", ci->CaptureFieldName), ind + 2, "valid = false; break");
        } else {
          out += std::format("{}    if (!s.skipValue()) {{ valid = false; break; }}\n", in);
          out += std::format("{}    valid = false; break;\n", in);
        }
      } else if (isStructRef(f, enumNames)) {
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
      } else if (f.Type.IsScalar && (f.Type.Scalar == EScalarType::Seconds ||
                 f.Type.Scalar == EScalarType::Minutes || f.Type.Scalar == EScalarType::Hours)) {
        std::string chronoType = cppScalarType(f.Type.Scalar);
        out += std::format("{}    {{ int64_t _t; if (!s.readInt64(_t)) {{ valid = false; break; }} "
                           "{}.push_back({}(_t)); }}\n", in, cn, chronoType);
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

    case EFieldKind::OptionalArray: {
      if (fieldAllowsNullInput(f))
        out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      else
        out += std::format("{}if (s.readNull()) {{ valid = false; }}\n", in);
      out += std::format("{}else {{\n", in);
      out += std::format("{}  {}.emplace();\n", in, cn);
      CFieldDef tmp = f;
      tmp.Name = std::format("(*{})", cn);
      tmp.Kind = EFieldKind::Array;
      generateParseField(out, tmp, enumNames, -1, ind + 1, false, ci);
      out += std::format("{}}}\n", in);
      if (foundBit >= 0)
        out += std::format("{}if (valid) found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::FixedArray: {
      out += std::format("{}if (!s.expectChar('[')) {{ valid = false; break; }}\n", in);
      out += std::format("{}for (size_t i_ = 0; i_ < {}; i_++) {{\n", in, f.Type.FixedSize);
      out += std::format("{}  if (i_ > 0 && !s.expectChar(',')) {{ valid = false; break; }}\n", in);

      if (!f.Type.InnerDims.empty()) {
        generateNestedArrayParse(out, f, enumNames, f.Type.InnerDims, 0,
                                 std::format("{}[i_]", cn),
                                 ci ? std::format("capture->{}[i_]", ci->CaptureFieldName) : "",
                                 ind + 1, ci);
      } else if (isMappedField(f)) {
        if (ci && ci->kind == CFieldCaptureInfo::MappedDirect) {
          generateDirectMappedCaptureRead(out, *ci, std::format("capture->{}[i_]", ci->CaptureFieldName), ind + 1, "valid = false; break");
        } else {
          generateParseMappedValue(out, f.Type.RefName, f.Type.MappedWireType,
                                   std::format("{}[i_]", cn), ind + 1, "valid = false; break");
        }
      } else if (isStructRef(f, enumNames)) {
        if (ci && ci->kind == CFieldCaptureInfo::NestedStruct) {
          out += std::format("{}  if (!{}[i_].parseImpl(s, capture ? &capture->{}[i_] : nullptr)) {{ valid = false; break; }}\n",
                             in, cn, ci->CaptureFieldName);
        } else {
          out += std::format("{}  if (!{}[i_].parseImpl(s)) {{ valid = false; break; }}\n", in, cn);
        }
      } else if (isEnum(f.Type.RefName, enumNames)) {
        out += std::format("{}  {{ const char *eStr; size_t eLen;\n", in);
        out += std::format("{}    if (!s.readString(eStr, eLen) || !parseE{}(eStr, eLen, {}[i_])) {{ valid = false; break; }} }}\n",
                           in, f.Type.RefName, cn);
      } else if (f.Type.IsScalar && (f.Type.Scalar == EScalarType::Seconds ||
                 f.Type.Scalar == EScalarType::Minutes || f.Type.Scalar == EScalarType::Hours)) {
        std::string chronoType = cppScalarType(f.Type.Scalar);
        out += std::format("{}  {{ int64_t _t; if (!s.readInt64(_t)) {{ valid = false; break; }} {}[i_] = {}(_t); }}\n",
                           in, cn, chronoType);
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
        out += std::format("{}  if (!s.{}({}[i_])) {{ valid = false; break; }}\n", in, readMethod, cn);
      }

      out += std::format("{}}}\n", in);
      out += std::format("{}if (!s.expectChar(']')) valid = false;\n", in);
      if (foundBit >= 0)
        out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::OptionalFixedArray: {
      if (fieldAllowsNullInput(f))
        out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      else
        out += std::format("{}if (s.readNull()) {{ valid = false; }}\n", in);
      out += std::format("{}else {{\n", in);
      out += std::format("{}  {}.emplace();\n", in, cn);
      CFieldDef tmp = f;
      tmp.Name = std::format("(*{})", cn);
      tmp.Kind = EFieldKind::FixedArray;
      generateParseField(out, tmp, enumNames, -1, ind + 1, false, ci);
      out += std::format("{}}}\n", in);
      if (foundBit >= 0)
        out += std::format("{}if (valid) found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::Variant: {
      out += std::format("{}s.skipWhitespace();\n", in);
      out += std::format("{}if (s.p >= s.end) {{ valid = false; break; }}\n", in);
      generateVariantParse(out, f, cn, enumNames, ind);
      if (foundBit >= 0)
        out += std::format("{}found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::OptionalVariant: {
      if (fieldAllowsNullInput(f))
        out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      else
        out += std::format("{}if (s.readNull()) {{ valid = false; }}\n", in);
      out += std::format("{}else {{\n", in);
      out += std::format("{}  s.skipWhitespace();\n", in);
      out += std::format("{}  if (s.p >= s.end) {{ valid = false; }}\n", in);
      out += std::format("{}  else {{\n", in);
      out += std::format("{}    {}.emplace();\n", in, cn);
      generateVariantParse(out, f, std::format("(*{})", cn), enumNames, ind + 2);
      out += std::format("{}  }}\n", in);
      out += std::format("{}}}\n", in);
      if (foundBit >= 0)
        out += std::format("{}if (valid) found |= (uint64_t)1 << {};\n", in, foundBit);
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
      std::string wireType = getWireTypeInfo(ci.MappedWireType).cppType;
      out += std::format("    {} {};\n", captureFieldType(f, wireType), ci.CaptureFieldName);
    } else if (ci.kind == CFieldCaptureInfo::NestedStruct) {
      std::string childCapture = opts.StructPrefix + f.Type.RefName + "::Capture";
      out += std::format("    {} {};\n", captureFieldType(f, childCapture), ci.CaptureFieldName);
    }
  }
}

static std::string capitalizeFirst(const std::string &s) {
  if (s.empty()) return s;
  std::string r = s;
  r[0] = toupper(r[0]);
  return r;
}

static std::string flatSerializeParamType(const CFieldDef &f,
    const std::unordered_set<std::string> &enumNames, const std::string &structPrefix)
{
  std::string type = cppFieldType(f, enumNames, structPrefix);
  // Enum types — pass by value (small, like int)
  if (!f.Type.RefName.empty() && enumNames.count(f.Type.RefName))
    return type;
  // Use std::string_view for plain string parameters
  if (f.Type.IsScalar && f.Type.Scalar == EScalarType::String && f.Kind == EFieldKind::Required)
    return "std::string_view";
  if (f.Type.IsScalar && f.Type.Scalar == EScalarType::String && f.Kind == EFieldKind::Optional)
    return "std::string_view";
  return "const " + type + " &";
}

static std::string flatSerializeParams(const CStructDef &s, const CContextAnalysis &ca,
    const std::unordered_set<std::string> &enumNames, const CCodegenOptions &opts)
{
  std::string params = "xmstream &out";
  for (auto &f : s.Fields) {
    std::string paramType = flatSerializeParamType(f, enumNames, opts.StructPrefix);
    std::string name = fieldCppName(f.Name, opts.PascalCaseFields);
    params += std::format(", {} {}", paramType, name);
  }
  if (ca.hasContext())
    params += ", " + ca.contextParams();
  return params;
}

static void generateStructDecl(std::string &out, const CStructDef &s,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts,
    const CContextAnalysis &ca)
{
  std::string sn = opts.StructPrefix + s.Name;
  bool needMembers = s.GenerateFlags.Parse || s.GenerateFlags.ParseVerbose || s.GenerateFlags.Serialize;
  bool hasBroadcastOverload = ca.hasContext() && ca.hasVectorContext();

  out += std::format("struct {} {{\n", sn);

  // Capture struct (always when Parse — empty for non-context structs)
  if (s.GenerateFlags.Parse) {
    out += "  struct Capture {\n";
    if (ca.hasContext())
      generateCaptureFields(out, s, ca, opts);
    out += "  };\n";
  }

  // Context type aliases — if context AND any codegen
  if (ca.hasContext() && (s.GenerateFlags.Parse || s.GenerateFlags.Serialize || s.GenerateFlags.SerializeFlat)) {
    std::unordered_set<std::string> emittedAliases;
    for (auto &n : ca.Needs) {
      if (emittedAliases.insert(n.MappedTypeName).second)
        out += std::format("  using {}Context = {};\n", capitalizeFirst(n.MappedTypeName), n.CppContextType);
    }
  }
  out += "\n";

  // Fields — only if parse or serialize (member-based)
  if (needMembers) {
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
  }

  // Parse/serialize methods
  if (s.GenerateFlags.Parse) {
    if (ca.hasContext()) {
      out += "  bool parse(const char *buf, size_t bufSize, Capture &capture);\n";
      out += "  bool parseImpl(JsonScanner &s, Capture *capture);\n";
      out += std::format("  static bool resolve({} &out, const Capture &capture, {});\n", sn, ca.contextParams());
      if (hasBroadcastOverload)
        out += std::format("  static bool resolve({} &out, const Capture &capture, {});\n", sn, ca.broadcastContextParams());
    } else {
      out += "  bool parse(const char *buf, size_t bufSize);\n";
      out += "  bool parseImpl(JsonScanner &s);\n";
    }
  }

  if (s.GenerateFlags.ParseVerbose && !ca.hasContext()) {
    out += "  bool parseVerbose(const char *buf, size_t bufSize, ParseError &error);\n";
    out += "  bool parseVerboseImpl(VerboseJsonScanner &s);\n";
  }

  if (s.GenerateFlags.Serialize) {
    if (ca.hasContext()) {
      out += std::format("  void serialize(xmstream &out, {}) const;\n", ca.contextParams());
      if (hasBroadcastOverload)
        out += std::format("  void serialize(xmstream &out, {}) const;\n", ca.broadcastContextParams());
    } else
      out += "  void serialize(xmstream &out) const;\n";
  }

  if (s.GenerateFlags.SerializeFlat) {
    std::string params = flatSerializeParams(s, ca, enumNames, opts);
    out += std::format("  static void serialize({});\n", params);
    if (hasBroadcastOverload) {
      CContextAnalysis broadcast = ca.broadcastVariant();
      std::string broadcastParams = flatSerializeParams(s, broadcast, enumNames, opts);
      out += std::format("  static void serialize({});\n", broadcastParams);
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

static void emitMappedDirectResolveRecursive(std::string &out,
    const CFieldCaptureInfo &ci,
    const std::vector<CArrayDim> &dims, int dimIndex,
    const std::string &valueExpr,
    const std::string &captureExpr,
    int ind)
{
  std::string in = indent(ind);
  if (dimIndex >= (int)dims.size()) {
    std::string resolveFunc = "__" + ci.MappedTypeName + "Resolve";
    out += std::format("{}if (!{}({}, {}, {})) return false;\n",
                       in, resolveFunc, captureExpr, ci.DirectCtxParamName, valueExpr);
    return;
  }

  std::string idx = std::format("i{}_r_", dimIndex);
  out += std::format("{}for (size_t {} = 0; {} < {}.size(); {}++) {{\n", in, idx, idx, valueExpr, idx);
  emitMappedDirectResolveRecursive(out, ci, dims, dimIndex + 1,
                                   std::format("{}[{}]", valueExpr, idx),
                                   std::format("{}[{}]", captureExpr, idx),
                                   ind + 1);
  out += std::format("{}}}\n", in);
}

static void emitNestedStructResolveRecursive(std::string &out,
    const std::string &childStruct,
    const CFieldCaptureInfo &ci,
    const std::vector<CArrayDim> &dims, int dimIndex,
    const std::string &valueExpr,
    const std::string &captureExpr,
    const std::string &ctxIndexVar,
    int ind)
{
  std::string in = indent(ind);
  if (dimIndex >= (int)dims.size()) {
    out += std::format("{}if (!{}::resolve({}, {}, {})) return false;\n",
                       in, childStruct, valueExpr, captureExpr, childCtxArgsExpr(ci, ctxIndexVar));
    if (!ctxIndexVar.empty())
      out += std::format("{}{}++;\n", in, ctxIndexVar);
    return;
  }

  std::string idx = std::format("i{}_r_", dimIndex);
  out += std::format("{}for (size_t {} = 0; {} < {}.size(); {}++) {{\n", in, idx, idx, valueExpr, idx);
  emitNestedStructResolveRecursive(out, childStruct, ci, dims, dimIndex + 1,
                                   std::format("{}[{}]", valueExpr, idx),
                                   std::format("{}[{}]", captureExpr, idx),
                                   ctxIndexVar, ind + 1);
  out += std::format("{}}}\n", in);
}

static void generateResolveImpl(std::string &out, const CStructDef &s,
    const CContextAnalysis &ca,
    const std::unordered_set<std::string> &,
    const CCodegenOptions &opts)
{
  std::string sn = opts.StructPrefix + s.Name;

  if (ca.hasContext())
    out += std::format("bool {}::resolve({} &out, const Capture &capture, {}) {{\n",
                       sn, sn, ca.contextParams());
  else
    out += std::format("bool {}::resolve({} &out, const Capture &capture) {{\n", sn, sn);

  for (size_t i = 0; i < s.Fields.size(); i++) {
    auto &ci = ca.FieldCapture[i];
    auto &f = s.Fields[i];
    std::string cn = fieldCppName(f.Name, opts.PascalCaseFields);
    auto dims = arrayDims(f);

    if (ci.kind == CFieldCaptureInfo::MappedDirect) {
      switch (f.Kind) {
        case EFieldKind::Required:
        case EFieldKind::Optional:
          emitMappedDirectResolveRecursive(out, ci, dims, 0, "out." + cn, "capture." + ci.CaptureFieldName, 1);
          break;
        case EFieldKind::OptionalObject:
          out += std::format("  if (out.{}.has_value()) {{\n", cn);
          emitMappedDirectResolveRecursive(out, ci, dims, 0,
                                           std::format("(*out.{})", cn),
                                           "capture." + ci.CaptureFieldName, 2);
          out += "  }\n";
          break;
        case EFieldKind::Array:
        case EFieldKind::FixedArray:
          emitMappedDirectResolveRecursive(out, ci, dims, 0, "out." + cn, "capture." + ci.CaptureFieldName, 1);
          break;
        case EFieldKind::OptionalArray:
        case EFieldKind::OptionalFixedArray:
          out += std::format("  if (out.{}.has_value()) {{\n", cn);
          emitMappedDirectResolveRecursive(out, ci, dims, 0,
                                           std::format("(*out.{})", cn),
                                           "capture." + ci.CaptureFieldName, 2);
          out += "  }\n";
          break;
        default:
          break;
      }
    } else if (ci.kind == CFieldCaptureInfo::NestedStruct) {
      std::string childStruct = opts.StructPrefix + f.Type.RefName;
      std::string childCtx = childCtxArgsExpr(ci, "");
      std::string ctxIndexVar;
      if (!dims.empty() && hasVectorChildContext(ci)) {
        ctxIndexVar = std::format("__ctxIdx{}_", i);
        out += std::format("  size_t {} = 0;\n", ctxIndexVar);
      }

      switch (f.Kind) {
        case EFieldKind::Required:
        case EFieldKind::Optional:
          if (dims.empty()) {
            out += std::format("  if (!{}::resolve(out.{}, capture.{}, {})) return false;\n",
                               childStruct, cn, ci.CaptureFieldName, childCtx);
          } else {
            emitNestedStructResolveRecursive(out, childStruct, ci, dims, 0,
                                             "out." + cn, "capture." + ci.CaptureFieldName,
                                             ctxIndexVar, 1);
          }
          break;
        case EFieldKind::OptionalObject:
          out += std::format("  if (out.{}.has_value()) {{\n", cn);
          out += std::format("    if (!{}::resolve(*out.{}, capture.{}, {})) return false;\n",
                             childStruct, cn, ci.CaptureFieldName, childCtx);
          out += "  }\n";
          break;
        case EFieldKind::Array:
        case EFieldKind::OptionalArray:
        case EFieldKind::FixedArray:
        case EFieldKind::OptionalFixedArray:
          if (f.Kind == EFieldKind::Array || f.Kind == EFieldKind::FixedArray) {
            emitNestedStructResolveRecursive(out, childStruct, ci, dims, 0,
                                             "out." + cn, "capture." + ci.CaptureFieldName,
                                             ctxIndexVar, 1);
          } else {
            out += std::format("  if (out.{}.has_value()) {{\n", cn);
            emitNestedStructResolveRecursive(out, childStruct, ci, dims, 0,
                                             std::format("(*out.{})", cn), "capture." + ci.CaptureFieldName,
                                             ctxIndexVar, 2);
            out += "  }\n";
          }
          break;
        default:
          break;
      }
    }
  }

  out += "  return true;\n";
  out += "}\n\n";
}

static void emitMappedDirectValueSerialize(CSerializeCodeBuilder &out,
    const CFieldCaptureInfo &fci,
    const std::string &valueExpr,
    int ind)
{
  auto wi = getWireTypeInfo(fci.MappedWireType);
  std::string in = indent(ind);
  std::string formatFunc = "__" + fci.MappedTypeName + "Format";
  out.appendRaw(std::format("{}{}(out, {}({}, {}));\n",
                            in, wi.writeFunc, formatFunc, valueExpr, fci.DirectCtxParamName));
}

static void emitMappedDirectSerializeRecursive(CSerializeCodeBuilder &out,
    const CFieldCaptureInfo &fci,
    const std::vector<CArrayDim> &dims, int dimIndex,
    const std::string &valueExpr,
    int ind)
{
  std::string in = indent(ind);
  if (dimIndex >= (int)dims.size()) {
    emitMappedDirectValueSerialize(out, fci, valueExpr, ind);
    return;
  }

  std::string idx = std::format("i{}_s_", dimIndex);
  out.appendRaw(std::format("{}for (size_t {} = 0; {} < {}.size(); {}++) {{\n", in, idx, idx, valueExpr, idx));
  out.appendRaw(std::format("{}  if ({}) out.write(',');\n", in, idx));
  emitMappedDirectSerializeRecursive(out, fci, dims, dimIndex + 1,
                                     std::format("{}[{}]", valueExpr, idx), ind + 1);
  out.appendRaw(std::format("{}}}\n", in));
}

static void emitNestedStructSerializeRecursive(CSerializeCodeBuilder &out,
    const CFieldCaptureInfo &ci,
    const std::vector<CArrayDim> &dims, int dimIndex,
    const std::string &valueExpr,
    const std::string &ctxIndexVar,
    int ind)
{
  std::string in = indent(ind);
  if (dimIndex >= (int)dims.size()) {
    out.appendRaw(std::format("{}{}.serialize(out, {});\n", in, valueExpr, childCtxArgsExpr(ci, ctxIndexVar)));
    if (!ctxIndexVar.empty())
      out.appendRaw(std::format("{}{}++;\n", in, ctxIndexVar));
    return;
  }

  std::string idx = std::format("i{}_s_", dimIndex);
  out.appendRaw(std::format("{}for (size_t {} = 0; {} < {}.size(); {}++) {{\n", in, idx, idx, valueExpr, idx));
  out.appendRaw(std::format("{}  if ({}) out.write(',');\n", in, idx));
  emitNestedStructSerializeRecursive(out, ci, dims, dimIndex + 1,
                                     std::format("{}[{}]", valueExpr, idx),
                                     ctxIndexVar, ind + 1);
  out.appendRaw(std::format("{}}}\n", in));
}

// Serialize a context-free mapped field using its format function
static void emitMappedInlineSerialize(CSerializeCodeBuilder &out, const CFieldDef &f,
    const CFieldCaptureInfo &fci, int ind, bool &first, bool pascalCase,
    bool useRuntimeComma = false, std::string_view commaVar = "")
{
  std::string in = indent(ind);
  std::string cn = fieldCppName(f.Name, pascalCase);

  auto emitKey = [&](const std::string &jsonKey, int keyInd = -1) {
    if (keyInd == -1)
      keyInd = ind;
    if (useRuntimeComma) {
      std::string keyIndent = indent(keyInd);
      out.appendRaw(std::format("{}if ({}) out.write(',');\n", keyIndent, commaVar));
      out.appendRaw(std::format("{}{} = true;\n", keyIndent, commaVar));
      out.writeLiteral(keyInd, std::format("\"{}\":", jsonKey));
      return;
    }

    if (!first)
      out.writeLiteral(keyInd, ",");
    first = false;
    out.writeLiteral(keyInd, std::format("\"{}\":", jsonKey));
  };

  switch (f.Kind) {
    case EFieldKind::Required:
      emitKey(f.Name);
      emitMappedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, cn, ind);
      break;
    case EFieldKind::Optional: {
      static const std::unordered_set<std::string> noEnums;
      std::string def = cppDefault(f, noEnums, pascalCase);
      if (def.empty()) {
        emitKey(f.Name);
        emitMappedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, cn, ind);
        break;
      }

      out.appendRaw(std::format("{}if ({} != {}) {{\n", in, cn, def));
      emitKey(f.Name, ind + 1);
      emitMappedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, cn, ind + 1);
      out.appendRaw(std::format("{}}}\n", in));
      break;
    }
    case EFieldKind::OptionalObject:
      if (!fieldEmptyOutIsNull(f)) {
        out.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
        emitKey(f.Name, ind + 1);
        emitMappedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("*{}", cn), ind + 1);
        out.appendRaw(std::format("{}}}\n", in));
        break;
      }
      emitKey(f.Name);
      out.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
      emitMappedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("*{}", cn), ind + 1);
      out.appendRaw(std::format("{}}} else {{\n", in));
      out.writeLiteral(ind + 1, "null");
      out.appendRaw(std::format("{}}}\n", in));
      break;
    case EFieldKind::Array:
      emitKey(f.Name);
      out.writeLiteral(ind, "[");
      out.appendRaw(std::format("{}for (size_t i_ = 0; i_ < {}.size(); i_++) {{\n", in, cn));
      out.appendRaw(std::format("{}  if (i_) out.write(',');\n", in));
      if (f.Type.InnerDims.empty()) {
        emitMappedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("{}[i_]", cn), ind + 1);
      } else {
        emitMappedInlineNestedArraySerialize(out, fci, f.Type.InnerDims, 0, std::format("{}[i_]", cn), ind + 1);
      }
      out.appendRaw(std::format("{}}}\n", in));
      out.writeLiteral(ind, "]");
      break;
    case EFieldKind::OptionalArray:
      if (!fieldEmptyOutIsNull(f)) {
        out.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
        emitKey(f.Name, ind + 1);
        out.writeLiteral(ind + 1, "[");
        out.appendRaw(std::format("{}  for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", in, cn));
        out.appendRaw(std::format("{}    if (i_) out.write(',');\n", in));
        if (f.Type.InnerDims.empty()) {
          emitMappedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("(*{})[i_]", cn), ind + 2);
        } else {
          emitMappedInlineNestedArraySerialize(out, fci, f.Type.InnerDims, 0, std::format("(*{})[i_]", cn), ind + 2);
        }
        out.appendRaw(std::format("{}  }}\n", in));
        out.writeLiteral(ind + 1, "]");
        out.appendRaw(std::format("{}}}\n", in));
        break;
      }
      emitKey(f.Name);
      out.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
      out.writeLiteral(ind + 1, "[");
      out.appendRaw(std::format("{}  for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", in, cn));
      out.appendRaw(std::format("{}    if (i_) out.write(',');\n", in));
      if (f.Type.InnerDims.empty()) {
        emitMappedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("(*{})[i_]", cn), ind + 2);
      } else {
        emitMappedInlineNestedArraySerialize(out, fci, f.Type.InnerDims, 0, std::format("(*{})[i_]", cn), ind + 2);
      }
      out.appendRaw(std::format("{}  }}\n", in));
      out.writeLiteral(ind + 1, "]");
      out.appendRaw(std::format("{}}} else {{\n", in));
      out.writeLiteral(ind + 1, "null");
      out.appendRaw(std::format("{}}}\n", in));
      break;
    case EFieldKind::FixedArray:
      emitKey(f.Name);
      out.writeLiteral(ind, "[");
      out.appendRaw(std::format("{}for (size_t i_ = 0; i_ < {}.size(); i_++) {{\n", in, cn));
      out.appendRaw(std::format("{}  if (i_) out.write(',');\n", in));
      if (f.Type.InnerDims.empty()) {
        emitMappedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("{}[i_]", cn), ind + 1);
      } else {
        emitMappedInlineNestedArraySerialize(out, fci, f.Type.InnerDims, 0, std::format("{}[i_]", cn), ind + 1);
      }
      out.appendRaw(std::format("{}}}\n", in));
      out.writeLiteral(ind, "]");
      break;
    case EFieldKind::OptionalFixedArray:
      if (!fieldEmptyOutIsNull(f)) {
        out.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
        emitKey(f.Name, ind + 1);
        out.writeLiteral(ind + 1, "[");
        out.appendRaw(std::format("{}  for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", in, cn));
        out.appendRaw(std::format("{}    if (i_) out.write(',');\n", in));
        if (f.Type.InnerDims.empty()) {
          emitMappedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("(*{})[i_]", cn), ind + 2);
        } else {
          emitMappedInlineNestedArraySerialize(out, fci, f.Type.InnerDims, 0, std::format("(*{})[i_]", cn), ind + 2);
        }
        out.appendRaw(std::format("{}  }}\n", in));
        out.writeLiteral(ind + 1, "]");
        out.appendRaw(std::format("{}}}\n", in));
        break;
      }
      emitKey(f.Name);
      out.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
      out.writeLiteral(ind + 1, "[");
      out.appendRaw(std::format("{}  for (size_t i_ = 0; i_ < {}->size(); i_++) {{\n", in, cn));
      out.appendRaw(std::format("{}    if (i_) out.write(',');\n", in));
      if (f.Type.InnerDims.empty()) {
        emitMappedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("(*{})[i_]", cn), ind + 2);
      } else {
        emitMappedInlineNestedArraySerialize(out, fci, f.Type.InnerDims, 0, std::format("(*{})[i_]", cn), ind + 2);
      }
      out.appendRaw(std::format("{}  }}\n", in));
      out.writeLiteral(ind + 1, "]");
      out.appendRaw(std::format("{}}} else {{\n", in));
      out.writeLiteral(ind + 1, "null");
      out.appendRaw(std::format("{}}}\n", in));
      break;
    default:
      break;
  }
}

// Generate the context-aware serialize body
static void generateContextSerializeBody(CSerializeCodeBuilder &writer, const CStructDef &s,
    const CContextAnalysis &ca,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts)
{
  bool first = true;
  bool runtimeComma = false;

  for (size_t i = 0; i < s.Fields.size(); i++) {
    auto &ci = ca.FieldCapture[i];
    auto &f = s.Fields[i];
    std::string in = indent(1);
    std::string cn = fieldCppName(f.Name, opts.PascalCaseFields);
    auto dims = arrayDims(f);

    if (!runtimeComma && first && isConditionallySerialized(f)) {
      writer.appendRaw(std::format("{}bool __needComma = {};\n", indent(1), first ? "false" : "true"));
      runtimeComma = true;
    }

    auto emitKey = [&](const std::string &jsonKey, int keyInd = 1) {
      if (runtimeComma) {
        std::string keyIndent = indent(keyInd);
        writer.appendRaw(std::format("{}if (__needComma) out.write(',');\n", keyIndent));
        writer.appendRaw(std::format("{}__needComma = true;\n", keyIndent));
        writer.writeLiteral(keyInd, std::format("\"{}\":", jsonKey));
        return;
      }
      if (!first)
        writer.writeLiteral(keyInd, ",");
      first = false;
      writer.writeLiteral(keyInd, std::format("\"{}\":", jsonKey));
    };

    if (ci.kind == CFieldCaptureInfo::MappedDirect) {
      switch (f.Kind) {
        case EFieldKind::Required:
          emitKey(f.Name);
          emitMappedDirectValueSerialize(writer, ci, cn, 1);
          break;
        case EFieldKind::Optional: {
          std::string def = cppDefault(f, enumNames, opts.PascalCaseFields);
          if (def.empty()) {
            emitKey(f.Name);
            emitMappedDirectValueSerialize(writer, ci, cn, 1);
            break;
          }

          writer.appendRaw(std::format("{}if ({} != {}) {{\n", in, cn, def));
          emitKey(f.Name, 2);
          emitMappedDirectValueSerialize(writer, ci, cn, 2);
          writer.appendRaw(std::format("{}}}\n", in));
          break;
        }
        case EFieldKind::OptionalObject:
          if (!fieldEmptyOutIsNull(f)) {
            writer.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
            emitKey(f.Name, 2);
            emitMappedDirectValueSerialize(writer, ci, std::format("*{}", cn), 2);
            writer.appendRaw(std::format("{}}}\n", in));
            break;
          }
          emitKey(f.Name);
          writer.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
          emitMappedDirectValueSerialize(writer, ci, std::format("*{}", cn), 2);
          writer.appendRaw(std::format("{}}} else {{\n", in));
          writer.writeLiteral(2, "null");
          writer.appendRaw(std::format("{}}}\n", in));
          break;
        case EFieldKind::Array:
        case EFieldKind::FixedArray:
          emitKey(f.Name);
          writer.writeLiteral(1, "[");
          emitMappedDirectSerializeRecursive(writer, ci, dims, 0, cn, 1);
          writer.writeLiteral(1, "]");
          break;
        case EFieldKind::OptionalArray:
        case EFieldKind::OptionalFixedArray: {
          if (!fieldEmptyOutIsNull(f)) {
            writer.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
            emitKey(f.Name, 2);
            writer.writeLiteral(2, "[");
            emitMappedDirectSerializeRecursive(writer, ci, dims, 0, std::format("(*{})", cn), 2);
            writer.writeLiteral(2, "]");
            writer.appendRaw(std::format("{}}}\n", in));
            break;
          }
          emitKey(f.Name);
          writer.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
          writer.writeLiteral(2, "[");
          emitMappedDirectSerializeRecursive(writer, ci, dims, 0, std::format("(*{})", cn), 2);
          writer.writeLiteral(2, "]");
          writer.appendRaw(std::format("{}}} else {{\n", in));
          writer.writeLiteral(2, "null");
          writer.appendRaw(std::format("{}}}\n", in));
          break;
        }
        default:
          break;
      }
      continue;
    }

    if (ci.kind == CFieldCaptureInfo::NestedStruct) {
      std::string childCtx = childCtxArgsExpr(ci, "");
      std::string ctxIndexVar;
      if (!dims.empty() && hasVectorChildContext(ci))
        ctxIndexVar = std::format("__ctxIdx{}_", i);
      switch (f.Kind) {
        case EFieldKind::Required:
        case EFieldKind::Optional:
          emitKey(f.Name);
          if (dims.empty()) {
            writer.appendRaw(std::format("{}{}.serialize(out, {});\n", in, cn, childCtx));
          } else {
            if (!ctxIndexVar.empty())
              writer.appendRaw(std::format("{}size_t {} = 0;\n", in, ctxIndexVar));
            writer.writeLiteral(1, "[");
            emitNestedStructSerializeRecursive(writer, ci, dims, 0, cn, ctxIndexVar, 1);
            writer.writeLiteral(1, "]");
          }
          break;
        case EFieldKind::OptionalObject: {
          if (!fieldEmptyOutIsNull(f)) {
            writer.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
            emitKey(f.Name, 2);
            writer.appendRaw(std::format("{}{}->serialize(out, {});\n", indent(2), cn, childCtx));
            writer.appendRaw(std::format("{}}}\n", in));
            break;
          }
          emitKey(f.Name);
          writer.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
          writer.appendRaw(std::format("{}  {}->serialize(out, {});\n", in, cn, childCtx));
          writer.appendRaw(std::format("{}}} else {{\n", in));
          writer.writeLiteral(2, "null");
          writer.appendRaw(std::format("{}}}\n", in));
          break;
        }
        case EFieldKind::Array:
        case EFieldKind::FixedArray:
          emitKey(f.Name);
          if (!ctxIndexVar.empty())
            writer.appendRaw(std::format("{}size_t {} = 0;\n", in, ctxIndexVar));
          writer.writeLiteral(1, "[");
          emitNestedStructSerializeRecursive(writer, ci, dims, 0, cn, ctxIndexVar, 1);
          writer.writeLiteral(1, "]");
          break;
        case EFieldKind::OptionalArray:
        case EFieldKind::OptionalFixedArray: {
          if (!fieldEmptyOutIsNull(f)) {
            writer.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
            emitKey(f.Name, 2);
            if (!ctxIndexVar.empty())
              writer.appendRaw(std::format("{}size_t {} = 0;\n", indent(2), ctxIndexVar));
            writer.writeLiteral(2, "[");
            emitNestedStructSerializeRecursive(writer, ci, dims, 0, std::format("(*{})", cn), ctxIndexVar, 2);
            writer.writeLiteral(2, "]");
            writer.appendRaw(std::format("{}}}\n", in));
            break;
          }
          emitKey(f.Name);
          writer.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
          if (!ctxIndexVar.empty())
            writer.appendRaw(std::format("{}size_t {} = 0;\n", indent(2), ctxIndexVar));
          writer.writeLiteral(2, "[");
          emitNestedStructSerializeRecursive(writer, ci, dims, 0, std::format("(*{})", cn), ctxIndexVar, 2);
          writer.writeLiteral(2, "]");
          writer.appendRaw(std::format("{}}} else {{\n", in));
          writer.writeLiteral(2, "null");
          writer.appendRaw(std::format("{}}}\n", in));
          break;
        }
        default:
          break;
      }
      continue;
    }

    // Regular field or context-free mapped field
    if (ci.kind == CFieldCaptureInfo::MappedInline)
      emitMappedInlineSerialize(writer, f, ci, 1, first, opts.PascalCaseFields, runtimeComma, "__needComma");
    else
      generateSerializeField(writer, f, enumNames, 1, first, opts.PascalCaseFields, runtimeComma, "__needComma");
  }
}

static void generateSerializeBody(CSerializeCodeBuilder &writer, const CStructDef &s,
    const CContextAnalysis &ca,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts)
{
  if (ca.hasContext()) {
    generateContextSerializeBody(writer, s, ca, enumNames, opts);
  } else {
    bool first = true;
    bool runtimeComma = false;
    for (size_t i = 0; i < s.Fields.size(); i++) {
      auto &fci = ca.FieldCapture[i];
      auto &f = s.Fields[i];
      if (!runtimeComma && first && isConditionallySerialized(f)) {
        writer.appendRaw(std::format("{}bool __needComma = {};\n", indent(1), first ? "false" : "true"));
        runtimeComma = true;
      }
      if (fci.kind == CFieldCaptureInfo::MappedInline) {
        emitMappedInlineSerialize(writer, f, fci, 1, first, opts.PascalCaseFields, runtimeComma, "__needComma");
      } else {
        generateSerializeField(writer, f, enumNames, 1, first, opts.PascalCaseFields, runtimeComma, "__needComma");
      }
    }
  }
}

static void generateStructImpl(std::string &out, const CStructDef &s,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts,
    const CContextAnalysis &ca)
{
  std::string sn = opts.StructPrefix + s.Name;
  bool hasCtx = ca.hasContext();
  bool hasBroadcastOverload = hasCtx && ca.hasVectorContext();
  bool needParse = s.GenerateFlags.Parse || s.GenerateFlags.ParseVerbose;

  // Compute perfect hash (needed for parse/parseVerbose)
  std::vector<std::string> fieldNames;
  std::optional<CPerfectHash> ph;
  std::unordered_map<uint32_t, size_t> hashToIndex;
  std::unordered_map<size_t, int> fieldBitMap;
  int requiredCount = 0;

  if (needParse) {
    for (auto &f : s.Fields)
      fieldNames.push_back(f.Name);

    ph = findPerfectHash(fieldNames);
    if (!ph) {
      fprintf(stderr, "warning: cannot find perfect hash for struct '%s', using mod=%zu\n",
              s.Name.c_str(), fieldNames.size() * 4);
      ph = CPerfectHash{0, 31, (uint32_t)(fieldNames.size() * 4)};
    }

    // Count required fields
    auto isRequired = [](const CFieldDef &f) {
      return f.Kind == EFieldKind::Required || f.Kind == EFieldKind::Array ||
             f.Kind == EFieldKind::FixedArray || f.Kind == EFieldKind::Variant ||
             fieldRequiresPresence(f);
    };
    for (auto &f : s.Fields)
      if (isRequired(f))
        requiredCount++;

    // Build hash -> field index map
    for (size_t i = 0; i < s.Fields.size(); i++) {
      uint32_t h = computeHash(s.Fields[i].Name.c_str(), s.Fields[i].Name.size(),
                                ph->Seed, ph->Mult, ph->Mod);
      hashToIndex[h] = i;
    }

    // Assign required field bits
    int bitIndex = 0;
    for (size_t i = 0; i < s.Fields.size(); i++)
      if (isRequired(s.Fields[i]))
        fieldBitMap[i] = bitIndex++;
  }

  // --- Parse ---
  if (s.GenerateFlags.Parse) {
    if (hasCtx) {
      out += std::format("bool {}::parse(const char *buf, size_t bufSize, Capture &capture) {{\n", sn);
      out += "  JsonScanner s{buf, buf + bufSize};\n";
      out += "  if (!parseImpl(s, &capture)) return false;\n";
      out += "  s.skipWhitespace();\n";
      out += "  return s.p == s.end;\n";
      out += "}\n\n";

      out += std::format("bool {}::parseImpl(JsonScanner &s, Capture *capture) {{\n", sn);
      generateParseBody(out, s, enumNames, opts, *ph, hashToIndex, fieldBitMap, requiredCount, &ca.FieldCapture);
      out += "}\n\n";
    } else {
      out += std::format("bool {}::parse(const char *buf, size_t bufSize) {{\n", sn);
      out += "  JsonScanner s{buf, buf + bufSize};\n";
      out += "  if (!parseImpl(s)) return false;\n";
      out += "  s.skipWhitespace();\n";
      out += "  return s.p == s.end;\n";
      out += "}\n\n";

      out += std::format("bool {}::parseImpl(JsonScanner &s) {{\n", sn);
      generateParseBody(out, s, enumNames, opts, *ph, hashToIndex, fieldBitMap, requiredCount, &ca.FieldCapture);
      out += "}\n\n";
    }
  }

  // --- Verbose parse ---
  if (s.GenerateFlags.ParseVerbose && !hasCtx) {
    out += std::format("bool {}::parseVerbose(const char *buf, size_t bufSize, ParseError &error) {{\n", sn);
    out += "  VerboseJsonScanner s{buf, buf + bufSize, buf, &error};\n";
    out += "  if (!parseVerboseImpl(s)) return false;\n";
    out += "  s.skipWhitespace();\n";
    out += "  if (s.p != s.end) {\n";
    out += "    s.setError(\"trailing characters after JSON value\");\n";
    out += "    return false;\n";
    out += "  }\n";
    out += "  return true;\n";
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
  if (s.GenerateFlags.Parse && hasCtx) {
    generateResolveImpl(out, s, ca, enumNames, opts);
    if (hasBroadcastOverload) {
      CContextAnalysis broadcast = ca.broadcastVariant();
      generateResolveImpl(out, s, broadcast, enumNames, opts);
    }
  }

  // --- serialize (member) ---
  if (s.GenerateFlags.Serialize) {
    if (hasCtx) {
      std::string ctxParams = ca.contextParams();
      out += std::format("void {}::serialize(xmstream &out, {}) const {{\n", sn, ctxParams);
    } else {
      out += std::format("void {}::serialize(xmstream &out) const {{\n", sn);
    }
    {
      CSerializeCodeBuilder writer(out);
      writer.writeLiteral(1, "{");
      generateSerializeBody(writer, s, ca, enumNames, opts);
      writer.writeLiteral(1, "}");
      writer.flush();
    }
    out += "}\n\n";

    if (hasBroadcastOverload) {
      CContextAnalysis broadcast = ca.broadcastVariant();
      out += std::format("void {}::serialize(xmstream &out, {}) const {{\n", sn, broadcast.broadcastContextParams());
      {
        CSerializeCodeBuilder writer(out);
        writer.writeLiteral(1, "{");
        generateSerializeBody(writer, s, broadcast, enumNames, opts);
        writer.writeLiteral(1, "}");
        writer.flush();
      }
      out += "}\n\n";
    }
  }

  // --- serialize (flat static) ---
  if (s.GenerateFlags.SerializeFlat) {
    std::string params = flatSerializeParams(s, ca, enumNames, opts);
    out += std::format("void {}::serialize({}) {{\n", sn, params);
    {
      CSerializeCodeBuilder writer(out);
      writer.writeLiteral(1, "{");
      generateSerializeBody(writer, s, ca, enumNames, opts);
      writer.writeLiteral(1, "}");
      writer.flush();
    }
    out += "}\n\n";

    if (hasBroadcastOverload) {
      CContextAnalysis broadcast = ca.broadcastVariant();
      std::string broadcastParams = flatSerializeParams(s, broadcast, enumNames, opts);
      out += std::format("void {}::serialize({}) {{\n", sn, broadcastParams);
      {
        CSerializeCodeBuilder writer(out);
        writer.writeLiteral(1, "{");
        generateSerializeBody(writer, s, broadcast, enumNames, opts);
        writer.writeLiteral(1, "}");
        writer.flush();
      }
      out += "}\n\n";
    }
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
  bool anyVerbose = false;
  bool anyFlatSerialize = false;
  bool hasFixedArray = false;
  bool hasVariant = false;
  for (auto &s : file.Structs) {
    if (!s.IsMixin && !s.IsImported) {
      if (s.HasTaggedSchema) hasTaggedSchema = true;
      if (s.GenerateFlags.ParseVerbose) anyVerbose = true;
      if (s.GenerateFlags.SerializeFlat) anyFlatSerialize = true;
      for (auto &f : s.Fields) {
        if (f.Type.IsScalar && (f.Type.Scalar == EScalarType::Seconds ||
            f.Type.Scalar == EScalarType::Minutes || f.Type.Scalar == EScalarType::Hours))
          hasChrono = true;
        if (f.Kind == EFieldKind::FixedArray || f.Kind == EFieldKind::OptionalFixedArray ||
            !f.Type.InnerDims.empty()) {
          for (auto &dim : f.Type.InnerDims)
            if (dim.FixedSize > 0) hasFixedArray = true;
          if (f.Type.FixedSize > 0) hasFixedArray = true;
        }
        if (!f.Type.Alternatives.empty())
          hasVariant = true;
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
  if (hasFixedArray)
    header += "#include <array>\n";
  if (hasVariant) {
    header += "#include <variant>\n";
    header += "#include <type_traits>\n";
  }
  if (anyFlatSerialize)
    header += "#include <string_view>\n";
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
  if (anyVerbose) {
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
  if (anyVerbose)
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
