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
  std::string MappedWireType;    // JSON wire type (e.g. "int64", "string") or usertype name
  bool WireIsUserType = false;   // true if MappedWireType is a usertype (not scalar)
  std::string WireUserTypeCppType; // C++ type when WireIsUserType
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

static bool hasStaticDefault(const CFieldDef &f)
{
  return f.Kind == EFieldKind::HasDefault &&
    f.Default.Kind != EDefaultKind::RuntimeNow &&
    f.Default.Kind != EDefaultKind::RuntimeNowOffset;
}

static bool isConditionallySerialized(const CFieldDef &f)
{
  if (isOptionalField(f) && !fieldEmptyOutIsNull(f))
    return true;
  return hasStaticDefault(f);
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
    const std::unordered_map<std::string, const CDerivedTypeDef *> &derivedTypes,
    const std::unordered_map<std::string, CContextAnalysis> &childAnalysis)
{
  CContextAnalysis a;
  a.FieldCapture.resize(s.Fields.size());

  std::vector<bool> fieldNeedsContext(s.Fields.size(), false);
  for (size_t i = 0; i < s.Fields.size(); i++) {
    auto &f = s.Fields[i];
    if (f.Type.IsDerived) {
      fieldNeedsContext[i] = true;
      continue;
    }

    if (!f.Type.RefName.empty() && !f.Type.IsScalar && !f.Type.IsUserType) {
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

    if (f.Type.IsUserType) {
      // Usertypes never have context — skip
      continue;
    }

    if (f.Type.IsDerived) {
      auto it = derivedTypes.find(f.Type.RefName);
      if (it != derivedTypes.end()) {
        a.FieldCapture[i].kind = CFieldCaptureInfo::MappedDirect;
        a.FieldCapture[i].CaptureFieldName = fieldCppName(f.Name, pascalCase);
        a.FieldCapture[i].MappedTypeName = f.Type.RefName;
        a.FieldCapture[i].MappedWireType = it->second->WireTypeName;
        a.FieldCapture[i].WireIsUserType = it->second->WireIsUserType;
        a.FieldCapture[i].WireUserTypeCppType = it->second->WireUserTypeCppType;
        if (hasGroup) {
          mergeGroupNeed(fieldGroupIt->second, f.Type.RefName, cppScalarType(it->second->ContextType), 0, false);
        }
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

static void markJsonHelperForScalar(CJsonHelperUsage &usage, EScalarType scalar)
{
  switch (scalar) {
    case EScalarType::String:
      usage.WriteString = true;
      break;
    case EScalarType::Bool:
      usage.WriteBool = true;
      break;
    case EScalarType::Int32:
    case EScalarType::Int64:
    case EScalarType::Seconds:
    case EScalarType::Minutes:
    case EScalarType::Hours:
      usage.WriteInt = true;
      break;
    case EScalarType::Uint32:
    case EScalarType::Uint64:
      usage.WriteUInt = true;
      break;
    case EScalarType::Double:
      usage.WriteDouble = true;
      break;
    case EScalarType::HexData:
      usage.WriteHexData = true;
      break;
  }
}

static void markJsonHelperForWireType(CJsonHelperUsage &usage, const std::string &wireType)
{
  if (wireType == "string")
    usage.WriteString = true;
  else if (wireType == "bool")
    usage.WriteBool = true;
  else if (wireType == "double")
    usage.WriteDouble = true;
  else if (wireType == "uint32" || wireType == "uint64")
    usage.WriteUInt = true;
  else
    usage.WriteInt = true;
}

static void collectSerializeHelpersForField(CJsonHelperUsage &usage,
    const CFieldDef &f,
    const CFieldCaptureInfo &ci,
    const std::unordered_set<std::string> &enumNames)
{
  if (!f.Type.Alternatives.empty()) {
    for (auto &alt : f.Type.Alternatives) {
      CFieldDef tmp = variantAltAsField(alt);
      collectSerializeHelpersForField(usage, tmp, CFieldCaptureInfo{}, enumNames);
    }
    return;
  }

  if (ci.kind == CFieldCaptureInfo::MappedDirect) {
    if (!ci.WireIsUserType)
      markJsonHelperForWireType(usage, ci.MappedWireType);
    return;
  }

  if (f.Type.IsUserType)
    return; // usertypes handle their own JSON I/O

  if (f.Type.IsDerived) {
    if (!f.Type.MappedWireIsUserType)
      markJsonHelperForWireType(usage, f.Type.MappedWireType);
    return;
  }

  if (isStructRef(f, enumNames) || isEnum(f.Type.RefName, enumNames))
    return;

  markJsonHelperForScalar(usage, f.Type.Scalar);
}

// --- Read usage tracking ---

static void markJsonReadForScalar(CJsonReadUsage &usage, EScalarType scalar)
{
  switch (scalar) {
    case EScalarType::String:
      usage.ReadString = true;
      break;
    case EScalarType::Bool:
      usage.ReadBool = true;
      break;
    case EScalarType::Int32:
    case EScalarType::Int64:
    case EScalarType::Seconds:
    case EScalarType::Minutes:
    case EScalarType::Hours:
      usage.ReadInt = true;
      break;
    case EScalarType::Uint32:
    case EScalarType::Uint64:
      usage.ReadUInt = true;
      break;
    case EScalarType::Double:
      usage.ReadDouble = true;
      break;
    case EScalarType::HexData:
      usage.ReadHexData = true;
      break;
  }
}

static void markJsonReadForWireType(CJsonReadUsage &usage, const std::string &wireType)
{
  if (wireType == "string")
    usage.ReadString = true;
  else if (wireType == "bool")
    usage.ReadBool = true;
  else if (wireType == "double")
    usage.ReadDouble = true;
  else if (wireType == "uint32" || wireType == "uint64")
    usage.ReadUInt = true;
  else
    usage.ReadInt = true;
}

static void collectParseReadUsageForField(CJsonReadUsage &usage,
    const CFieldDef &f,
    const std::unordered_set<std::string> &enumNames)
{
  if (!f.Type.Alternatives.empty()) {
    for (auto &alt : f.Type.Alternatives) {
      CFieldDef tmp = variantAltAsField(alt);
      collectParseReadUsageForField(usage, tmp, enumNames);
    }
    return;
  }

  if (f.Type.IsUserType)
    return; // usertypes handle their own JSON I/O

  if (f.Type.IsDerived) {
    if (!f.Type.MappedWireIsUserType)
      markJsonReadForWireType(usage, f.Type.MappedWireType);
    return;
  }

  if (isStructRef(f, enumNames) || isEnum(f.Type.RefName, enumNames))
    return;

  markJsonReadForScalar(usage, f.Type.Scalar);
}

// Wire type info for context-free mapped types — WireTypeInfo/getWireTypeInfo in codegenCommon.h

static void generateDirectMappedCaptureRead(std::string &out,
                                            const CFieldCaptureInfo &ci,
                                            const std::string &captureExpr,
                                            int ind,
                                            const std::string &failureCode)
{
  std::string in = indent(ind);
  out += std::format("{}if (capture) {{\n", in);
  if (ci.WireIsUserType) {
    std::string parseName = "__" + ci.MappedWireType + "Parse";
    out += std::format("{}  if ({}(s.p, s.end, {}) != JsonReadError::Ok) {{ {}; }}\n", in, parseName, captureExpr, failureCode);
  } else {
    auto wi = getWireTypeInfo(ci.MappedWireType);
    out += std::format("{}  if (s.{}({}) != JsonReadError::Ok) {{ {}; }}\n", in, wi.readMethod, captureExpr, failureCode);
  }
  out += std::format("{}}} else {{\n", in);
  out += std::format("{}  if (!s.skipValue()) {{ {}; }}\n", in, failureCode);
  out += std::format("{}}}\n", in);
}

static void generateParseMappedValue(std::string &out,
                                     const std::string &mappedTypeName,
                                     const std::string &mappedWireType,
                                     bool wireIsUserType,
                                     const std::string &wireUserTypeCppType,
                                     const std::string &destExpr,
                                     int ind,
                                     const std::string &failureCode)
{
  std::string in = indent(ind);
  std::string resolveName = "__" + mappedTypeName + "Resolve";
  if (wireIsUserType) {
    std::string parseName = "__" + mappedWireType + "Parse";
    out += std::format("{}{{ {} _tmp; if ({}(s.p, s.end, _tmp) != JsonReadError::Ok || !{}(_tmp, {})) {{ {}; }} }}\n",
                       in, wireUserTypeCppType, parseName, resolveName, destExpr, failureCode);
  } else {
    auto wi = getWireTypeInfo(mappedWireType);
    out += std::format("{}{{ {} _tmp; if (s.{}(_tmp) != JsonReadError::Ok || !{}(_tmp, {})) {{ {}; }} }}\n",
                       in, wi.cppType, wi.readMethod, resolveName, destExpr, failureCode);
  }
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

  // Usertype field — direct parse
  if (f.Type.IsUserType) {
    std::string readName = "__" + f.Type.RefName + "Parse";
    out += std::format("{}if ({}(s.p, s.end, {}) != JsonReadError::Ok) valid = false;\n", in, readName, cn);
    if (foundBit >= 0)
      out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
    return;
  }

  // Derived field — wire type read + resolve
  if (isDerivedField(f)) {
    if (ci && ci->kind == CFieldCaptureInfo::MappedDirect) {
      out += std::format("{}if (capture) {{\n", in);
      if (ci->WireIsUserType) {
        std::string parseName = "__" + ci->MappedWireType + "Parse";
        out += std::format("{}  if ({}(s.p, s.end, capture->{}) != JsonReadError::Ok) valid = false;\n", in, parseName, ci->CaptureFieldName);
      } else {
        auto wi = getWireTypeInfo(ci->MappedWireType);
        out += std::format("{}  if (s.{}(capture->{}) != JsonReadError::Ok) valid = false;\n", in, wi.readMethod, ci->CaptureFieldName);
      }
      if (foundBit >= 0)
        out += std::format("{}  else found |= (uint64_t)1 << {};\n", in, foundBit);
      out += std::format("{}}} else {{\n", in);
      out += std::format("{}  if (!s.skipValue()) valid = false;\n", in);
      if (foundBit >= 0)
        out += std::format("{}  else found |= (uint64_t)1 << {};\n", in, foundBit);
      out += std::format("{}}}\n", in);
    } else {
      std::string resolveName = "__" + f.Type.RefName + "Resolve";
      if (f.Type.MappedWireIsUserType) {
        std::string parseName = "__" + f.Type.MappedWireType + "Parse";
        out += std::format("{}{{ {} _tmp; if ({}(s.p, s.end, _tmp) != JsonReadError::Ok || !{}(_tmp, {})) valid = false;\n",
                           in, f.Type.MappedWireUserTypeCppType, parseName, resolveName, cn);
      } else {
        auto wi = getWireTypeInfo(f.Type.MappedWireType);
        out += std::format("{}{{ {} _tmp; if (s.{}(_tmp) != JsonReadError::Ok || !{}(_tmp, {})) valid = false;\n",
                           in, wi.cppType, wi.readMethod, resolveName, cn);
      }
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
    out += std::format("{}{{ int64_t _t; if (s.readInt64(_t) != JsonReadError::Ok) valid = false;\n", in);
    out += std::format("{}  else {{ {} = {}(_t);\n", in, cn, chronoType);
    if (foundBit >= 0)
      out += std::format("{}    found |= (uint64_t)1 << {};\n", in, foundBit);
    out += std::format("{}  }} }}\n", in);
    return;
  }

  // HexData type
  if (f.Type.IsScalar && f.Type.Scalar == EScalarType::HexData) {
    out += std::format("{}if (jsonReadHexData(s.p, s.end, {}) != JsonReadError::Ok) valid = false;\n", in, cn);
    if (foundBit >= 0)
      out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
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

  out += std::format("{}if (s.{}({}) != JsonReadError::Ok) valid = false;\n", in, readMethod, cn);
  if (foundBit >= 0)
    out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
}

// Forward declarations for functions used in nested array parse
static void generateVariantParse(std::string &out, const CFieldDef &f,
    const std::string &varName,
    const std::unordered_set<std::string> &enumNames, int ind);

// Parse a plain (non-optional/non-variant/non-map) leaf array element
static void generateParsePlainLeafElement(std::string &out, const CFieldDef &f,
    const std::unordered_set<std::string> &enumNames,
    const std::string &containerExpr,
    const std::string &captureExpr,
    int ind,
    const CFieldCaptureInfo *ci)
{
  std::string in = indent(ind);
  if (f.Type.IsUserType) {
    std::string readName = "__" + f.Type.RefName + "Parse";
    out += std::format("{}if ({}(s.p, s.end, {}) != JsonReadError::Ok) {{ valid = false; break; }}\n", in, readName, containerExpr);
  } else if (isDerivedField(f)) {
    if (ci && ci->kind == CFieldCaptureInfo::MappedDirect) {
      generateDirectMappedCaptureRead(out, *ci, captureExpr, ind, "valid = false; break");
    } else {
      generateParseMappedValue(out, f.Type.RefName, f.Type.MappedWireType, f.Type.MappedWireIsUserType, f.Type.MappedWireUserTypeCppType, containerExpr, ind, "valid = false; break");
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
    out += std::format("{}{{ int64_t _t; if (s.readInt64(_t) != JsonReadError::Ok) {{ valid = false; break; }} {} = {}(_t); }}\n",
                       in, containerExpr, chronoType);
  } else if (f.Type.IsScalar && f.Type.Scalar == EScalarType::HexData) {
    out += std::format("{}if (jsonReadHexData(s.p, s.end, {}) != JsonReadError::Ok) {{ valid = false; break; }}\n", in, containerExpr);
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
    out += std::format("{}if (s.{}({}) != JsonReadError::Ok) {{ valid = false; break; }}\n", in, readMethod, containerExpr);
  }
}

// Parse a map array element: { "key": value, ... }
static void generateParseMapElement(std::string &out, const CFieldDef &f,
    const std::unordered_set<std::string> &enumNames,
    const std::string &containerExpr, int ind)
{
  std::string in = indent(ind);
  out += std::format("{}if (!s.expectChar('{{')) {{ valid = false; break; }}\n", in);
  out += std::format("{}s.skipWhitespace();\n", in);
  out += std::format("{}if (s.p < s.end && *s.p != '}}') {{\n", in);
  out += std::format("{}  for (;;) {{\n", in);
  out += std::format("{}    const char *_mapKey; size_t _mapKeyLen;\n", in);
  out += std::format("{}    if (!s.readString(_mapKey, _mapKeyLen)) {{ valid = false; break; }}\n", in);
  out += std::format("{}    if (!s.expectChar(':')) {{ valid = false; break; }}\n", in);
  out += std::format("{}    s.skipWhitespace();\n", in);
  out += std::format("{}    auto &_mapVal = {}[std::string(_mapKey, _mapKeyLen)];\n", in, containerExpr);

  if (isStructRef(f, enumNames)) {
    out += std::format("{}    if (!_mapVal.parseImpl(s)) {{ valid = false; break; }}\n", in);
  } else if (isEnum(f.Type.RefName, enumNames)) {
    out += std::format("{}    {{ const char *eStr; size_t eLen;\n", in);
    out += std::format("{}      if (!s.readString(eStr, eLen) || !parseE{}(eStr, eLen, _mapVal)) {{ valid = false; break; }} }}\n",
                       in, f.Type.RefName);
  } else if (f.Type.IsScalar && (f.Type.Scalar == EScalarType::Seconds ||
             f.Type.Scalar == EScalarType::Minutes || f.Type.Scalar == EScalarType::Hours)) {
    std::string chronoType = cppScalarType(f.Type.Scalar);
    out += std::format("{}    {{ int64_t _t; if (s.readInt64(_t) != JsonReadError::Ok) {{ valid = false; break; }} _mapVal = {}(_t); }}\n",
                       in, chronoType);
  } else if (f.Type.IsScalar && f.Type.Scalar == EScalarType::HexData) {
    out += std::format("{}    if (jsonReadHexData(s.p, s.end, _mapVal) != JsonReadError::Ok) {{ valid = false; break; }}\n", in);
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
    out += std::format("{}    if (s.{}(_mapVal) != JsonReadError::Ok) {{ valid = false; break; }}\n", in, readMethod);
  }

  out += std::format("{}    s.skipWhitespace();\n", in);
  out += std::format("{}    if (s.p < s.end && *s.p == ',') {{ s.p++; s.skipWhitespace(); continue; }}\n", in);
  out += std::format("{}    break;\n", in);
  out += std::format("{}  }}\n", in);
  out += std::format("{}}}\n", in);
  out += std::format("{}if (!s.expectCharNoWs('}}')) {{ valid = false; }}\n", in);
  out += std::format("{}if (!valid) break;\n", in);
}

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
    // Leaf element — dispatch by array element kind
    switch (f.Type.ArrayElementKind) {
      case EArrayElementKind::Optional: {
        bool allowNull = (f.Type.ElementNullIn != ENullInPolicy::Deny);
        if (allowNull) {
          out += std::format("{}if (s.readNull()) {{ /* ok, stays nullopt */ }}\n", in);
          out += std::format("{}else {{\n", in);
        } else {
          out += std::format("{}if (s.readNull()) {{ valid = false; break; }}\n", in);
          out += std::format("{}{{\n", in);
        }
        out += std::format("{}  {}.emplace();\n", in, containerExpr);
        generateParsePlainLeafElement(out, f, enumNames,
                                      std::format("(*{})", containerExpr), captureExpr, ind + 1, ci);
        out += std::format("{}}}\n", in);
        return;
      }
      case EArrayElementKind::Variant:
        generateVariantParse(out, f, containerExpr, enumNames, ind);
        out += std::format("{}if (!valid) break;\n", in);
        return;
      case EArrayElementKind::Map:
        generateParseMapElement(out, f, enumNames, containerExpr, ind);
        return;
      default:
        break;
    }
    // Plain element
    generateParsePlainLeafElement(out, f, enumNames, containerExpr, captureExpr, ind, ci);
    return;
  }

  auto &dim = dims[dimIndex];
  std::string idx = std::format("i{}_", dimIndex);

  if (dim.FixedSize > 0) {
    // Fixed inner dimension
    out += std::format("{}if (!s.expectChar('[')) {{ valid = false; break; }}\n", in);
    out += std::format("{}for (size_t {} = 0; {} < {}; {}++) {{\n", in, idx, idx, dim.FixedSize, idx);
    out += std::format("{}  if ({} > 0 && !s.expectChar(',')) {{ valid = false; break; }}\n", in, idx);
    out += std::format("{}  s.skipWhitespace();\n", in);
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
    out += std::format("{}    if (s.p < s.end && *s.p == ',') {{ s.p++; s.skipWhitespace(); continue; }}\n", in);
    out += std::format("{}    break;\n", in);
    out += std::format("{}  }}\n", in);
    out += std::format("{}}}\n", in);
    out += std::format("{}if (!s.expectCharNoWs(']')) {{ valid = false; break; }}\n", in);
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
    case EFieldKind::HasDefault: {
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
                            ((isDerivedField(f) || f.Type.IsUserType) && ci) ? ci : nullptr);
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
        generateParseScalar(out, tmp, enumNames, -1, ind + 1, false, ((isDerivedField(f) || f.Type.IsUserType) && ci) ? ci : nullptr);
        out += std::format("{}}}\n", in);
      }
      if (foundBit >= 0)
        out += std::format("{}if (valid) found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::Array: {
      if (!f.Type.InnerDims.empty() || f.Type.ArrayElementKind != EArrayElementKind::Plain) {
        // Multi-dimensional array or complex element kind: parse via recursive function
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
        out += std::format("{}    if (s.p < s.end && *s.p == ',') {{ s.p++; s.skipWhitespace(); continue; }}\n", in);
        out += std::format("{}    break;\n", in);
        out += std::format("{}  }}\n", in);
        out += std::format("{}}}\n", in);
        out += std::format("{}if (!s.expectCharNoWs(']')) valid = false;\n", in);
        if (foundBit >= 0)
          out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
        break;
      }

      out += std::format("{}if (!s.expectChar('[')) {{ valid = false; break; }}\n", in);
      out += std::format("{}s.skipWhitespace();\n", in);
      out += std::format("{}if (s.p < s.end && *s.p != ']') {{\n", in);
      out += std::format("{}  for (;;) {{\n", in);

      if (f.Type.IsUserType) {
        std::string readName = "__" + f.Type.RefName + "Parse";
        out += std::format("{}    {}.emplace_back(); if ({}(s.p, s.end, {}.back()) != JsonReadError::Ok) {{ valid = false; break; }}\n",
                           in, cn, readName, cn);
      } else if (isDerivedField(f)) {
        if (ci && ci->kind == CFieldCaptureInfo::MappedDirect) {
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
        out += std::format("{}    {{ int64_t _t; if (s.readInt64(_t) != JsonReadError::Ok) {{ valid = false; break; }} "
                           "{}.push_back({}(_t)); }}\n", in, cn, chronoType);
      } else if (f.Type.IsScalar && f.Type.Scalar == EScalarType::HexData) {
        out += std::format("{}    {{ std::vector<uint8_t> tmp; if (jsonReadHexData(s.p, s.end, tmp) != JsonReadError::Ok) {{ valid = false; break; }} "
                           "{}.push_back(std::move(tmp)); }}\n", in, cn);
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
          out += std::format("{}    {{ std::string tmp; if (s.{}(tmp) != JsonReadError::Ok) {{ valid = false; break; }} "
                             "{}.push_back(std::move(tmp)); }}\n", in, readMethod, cn);
        } else {
          out += std::format("{}    {{ {} tmp; if (s.{}(tmp) != JsonReadError::Ok) {{ valid = false; break; }} "
                             "{}.push_back(tmp); }}\n", in, cppType, readMethod, cn);
        }
      }

      out += std::format("{}    s.skipWhitespace();\n", in);
      out += std::format("{}    if (s.p < s.end && *s.p == ',') {{ s.p++; s.skipWhitespace(); continue; }}\n", in);
      out += std::format("{}    break;\n", in);
      out += std::format("{}  }}\n", in);
      out += std::format("{}}}\n", in);
      out += std::format("{}if (!s.expectCharNoWs(']')) valid = false;\n", in);
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
      out += std::format("{}  s.skipWhitespace();\n", in);

      if (!f.Type.InnerDims.empty() || f.Type.ArrayElementKind != EArrayElementKind::Plain) {
        generateNestedArrayParse(out, f, enumNames, f.Type.InnerDims, 0,
                                 std::format("{}[i_]", cn),
                                 ci ? std::format("capture->{}[i_]", ci->CaptureFieldName) : "",
                                 ind + 1, ci);
      } else if (f.Type.IsUserType) {
        std::string readName = "__" + f.Type.RefName + "Parse";
        out += std::format("{}  if ({}(s.p, s.end, {}[i_]) != JsonReadError::Ok) {{ valid = false; break; }}\n", in, readName, cn);
      } else if (isDerivedField(f)) {
        if (ci && ci->kind == CFieldCaptureInfo::MappedDirect) {
          generateDirectMappedCaptureRead(out, *ci, std::format("capture->{}[i_]", ci->CaptureFieldName), ind + 1, "valid = false; break");
        } else {
          generateParseMappedValue(out, f.Type.RefName, f.Type.MappedWireType, f.Type.MappedWireIsUserType, f.Type.MappedWireUserTypeCppType,
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
        out += std::format("{}  {{ int64_t _t; if (s.readInt64(_t) != JsonReadError::Ok) {{ valid = false; break; }} {}[i_] = {}(_t); }}\n",
                           in, cn, chronoType);
      } else if (f.Type.IsScalar && f.Type.Scalar == EScalarType::HexData) {
        out += std::format("{}  if (jsonReadHexData(s.p, s.end, {}[i_]) != JsonReadError::Ok) {{ valid = false; break; }}\n", in, cn);
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
        out += std::format("{}  if (s.{}({}[i_]) != JsonReadError::Ok) {{ valid = false; break; }}\n", in, readMethod, cn);
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

    case EFieldKind::Map: {
      out += std::format("{}if (!s.expectChar('{{')) {{ valid = false; break; }}\n", in);
      out += std::format("{}s.skipWhitespace();\n", in);
      out += std::format("{}if (s.p < s.end && *s.p != '}}') {{\n", in);
      out += std::format("{}  for (;;) {{\n", in);
      out += std::format("{}    const char *_mapKey; size_t _mapKeyLen;\n", in);
      out += std::format("{}    if (!s.readString(_mapKey, _mapKeyLen)) {{ valid = false; break; }}\n", in);
      out += std::format("{}    if (!s.expectChar(':')) {{ valid = false; break; }}\n", in);
      out += std::format("{}    s.skipWhitespace();\n", in);
      out += std::format("{}    auto &_mapVal = {}[std::string(_mapKey, _mapKeyLen)];\n", in, cn);

      if (isStructRef(f, enumNames)) {
        out += std::format("{}    if (!_mapVal.parseImpl(s)) {{ valid = false; break; }}\n", in);
      } else if (isEnum(f.Type.RefName, enumNames)) {
        out += std::format("{}    {{ const char *eStr; size_t eLen;\n", in);
        out += std::format("{}      if (!s.readString(eStr, eLen) || !parseE{}(eStr, eLen, _mapVal)) {{ valid = false; break; }} }}\n",
                           in, f.Type.RefName);
      } else if (f.Type.IsScalar && (f.Type.Scalar == EScalarType::Seconds ||
                 f.Type.Scalar == EScalarType::Minutes || f.Type.Scalar == EScalarType::Hours)) {
        std::string chronoType = cppScalarType(f.Type.Scalar);
        out += std::format("{}    {{ int64_t _t; if (s.readInt64(_t) != JsonReadError::Ok) {{ valid = false; break; }} _mapVal = {}(_t); }}\n",
                           in, chronoType);
      } else if (f.Type.IsScalar && f.Type.Scalar == EScalarType::HexData) {
        out += std::format("{}    if (jsonReadHexData(s.p, s.end, _mapVal) != JsonReadError::Ok) {{ valid = false; break; }}\n", in);
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
        out += std::format("{}    if (s.{}(_mapVal) != JsonReadError::Ok) {{ valid = false; break; }}\n", in, readMethod);
      }

      out += std::format("{}    s.skipWhitespace();\n", in);
      out += std::format("{}    if (s.p < s.end && *s.p == ',') {{ s.p++; s.skipWhitespace(); continue; }}\n", in);
      out += std::format("{}    break;\n", in);
      out += std::format("{}  }}\n", in);
      out += std::format("{}}}\n", in);
      out += std::format("{}if (!s.expectCharNoWs('}}')) valid = false;\n", in);
      if (foundBit >= 0)
        out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
      break;
    }

    case EFieldKind::OptionalMap: {
      if (fieldAllowsNullInput(f))
        out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      else
        out += std::format("{}if (s.readNull()) {{ valid = false; }}\n", in);
      out += std::format("{}else {{\n", in);
      out += std::format("{}  {}.emplace();\n", in, cn);
      CFieldDef tmp = f;
      tmp.Name = std::format("(*{})", cn);
      tmp.Kind = EFieldKind::Map;
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

// Generate parse body for toplevel structs (single field, no struct wrapper)
static void generateTopLevelParseBody(std::string &out, const CStructDef &s,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts,
    const std::vector<CFieldCaptureInfo> *fieldCapture = nullptr)
{
  auto &f = s.Fields[0];
  const CFieldCaptureInfo *ci = nullptr;
  if (fieldCapture && !fieldCapture->empty() && (*fieldCapture)[0].kind != CFieldCaptureInfo::None)
    ci = &(*fieldCapture)[0];

  out += "  bool valid = true;\n";
  out += "  do {\n";
  generateParseField(out, f, enumNames, -1, 2, opts.PascalCaseFields, ci);
  out += "  } while(false);\n";
  out += "  return valid;\n";
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
    if (s.SkipUnknownFields) {
      out += "      if (!s.skipValue()) return false;\n";
    } else {
      out += "      return false;\n";
    }
  } else {
    out += "      uint32_t keyHash;\n";
    out += std::format("      if (!s.readStringHash(key, keyLen, keyHash, {}, {})) return false;\n",
                       ph.Seed, ph.Mult);
    out += "      if (!s.expectChar(':')) return false;\n";
    out += "      s.skipWhitespace();\n";
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
      if (s.SkipUnknownFields) {
        out += "          } else { if (!s.skipValue()) return false; }\n";
      } else {
        out += "          } else { return false; }\n";
      }
      out += "          break;\n";
    }

    out += "        default:\n";
    if (s.SkipUnknownFields) {
      out += "          if (!s.skipValue()) return false;\n";
    } else {
      out += "          return false;\n";
    }
    out += "      }\n";
  }

  out += "      s.skipWhitespace();\n";
  out += "      if (s.p < s.end && *s.p == ',') { s.p++; continue; }\n";
  out += "      break;\n";
  out += "    }\n";
  out += "  }\n";
  out += "  if (!s.expectCharNoWs('}')) return false;\n";

  if (requiredCount > 0) {
    uint64_t mask = ((uint64_t)1 << requiredCount) - 1;
    out += std::format("  return valid && (found & 0x{:x}) == 0x{:x};\n", mask, mask);
  } else {
    out += "  return valid;\n";
  }
}

// Generate parse body for array_layout structs (JSON array, positional fields)
static void generateArrayLayoutParseBody(std::string &out, const CStructDef &s,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts,
    const std::unordered_map<size_t, int> &fieldBitMap,
    int requiredCount)
{
  out += "  if (!s.expectChar('[')) return false;\n";
  out += "  bool valid = true;\n";
  if (requiredCount > 0)
    out += "  uint64_t found = 0;\n";
  out += "  s.skipWhitespace();\n";

  if (s.Fields.empty()) {
    if (s.SkipUnknownFields) {
      out += "  if (s.p < s.end && *s.p != ']') {\n";
      out += "    for (;;) {\n";
      out += "      if (!s.skipValue()) return false;\n";
      out += "      s.skipWhitespace();\n";
      out += "      if (s.p < s.end && *s.p == ',') { s.p++; s.skipWhitespace(); continue; }\n";
      out += "      break;\n";
      out += "    }\n";
      out += "  }\n";
    }
    out += "  if (!s.expectChar(']')) return false;\n";
    out += "  return valid;\n";
    return;
  }

  // Find boundary: required fields [0..firstTrailing), optional/default [firstTrailing..N)
  size_t firstTrailing = s.Fields.size();
  for (size_t i = 0; i < s.Fields.size(); i++) {
    if (s.Fields[i].Kind == EFieldKind::HasDefault || isOptionalField(s.Fields[i])) {
      firstTrailing = i;
      break;
    }
  }

  // For firstTrailing == 0 (all fields optional): check if array is non-empty
  // For firstTrailing > 0: first element is mandatory; required fields use expectChar(',')
  out += "  if (s.p < s.end && *s.p != ']') {\n";

  // Indentation level increases with nesting for trailing fields
  int baseInd = 2; // inside the "if non-empty" block

  // Emit required fields: each one (except first) demands comma
  for (size_t i = 0; i < firstTrailing; i++) {
    auto &f = s.Fields[i];
    int foundBit = -1;
    auto bitIt = fieldBitMap.find(i);
    if (bitIt != fieldBitMap.end())
      foundBit = bitIt->second;

    if (i > 0) {
      std::string in0 = indent(baseInd);
      out += std::format("{}if (!s.expectChar(',')) {{ valid = false; }}\n", in0);
      out += std::format("{}else {{\n", in0);
      out += std::format("{}s.skipWhitespace();\n", indent(baseInd + 1));
      baseInd++;
    }

    // Wrap in do-while(false) so that break statements in parse code work
    out += std::format("{}do {{\n", indent(baseInd));
    generateParseField(out, f, enumNames, foundBit, baseInd, opts.PascalCaseFields, nullptr);
    out += std::format("{}}} while (false);\n", indent(baseInd));
  }

  // After last required field (or from the start if no required fields), emit trailing fields
  int trailingBaseInd = baseInd;
  for (size_t i = firstTrailing; i < s.Fields.size(); i++) {
    auto &f = s.Fields[i];
    int foundBit = -1;
    auto bitIt = fieldBitMap.find(i);
    if (bitIt != fieldBitMap.end())
      foundBit = bitIt->second;

    std::string in0 = indent(baseInd);
    if (i == 0) {
      // First element in array — no comma needed, just parse the value
      // (already inside "if non-empty" block)
    } else {
      // Subsequent trailing element — check for comma
      out += std::format("{}s.skipWhitespace();\n", in0);
      out += std::format("{}if (s.p < s.end && *s.p == ',') {{\n", in0);
      out += std::format("{}s.p++; s.skipWhitespace();\n", indent(baseInd + 1));
      baseInd++;
    }

    out += std::format("{}do {{\n", indent(baseInd));
    generateParseField(out, f, enumNames, foundBit, baseInd, opts.PascalCaseFields, nullptr);
    out += std::format("{}}} while (false);\n", indent(baseInd));
  }

  // Skip unknown trailing elements
  if (s.SkipUnknownFields) {
    std::string in0 = indent(baseInd);
    out += std::format("{}s.skipWhitespace();\n", in0);
    out += std::format("{}while (s.p < s.end && *s.p == ',') {{\n", in0);
    out += std::format("{}  s.p++; s.skipWhitespace();\n", in0);
    out += std::format("{}  if (!s.skipValue()) {{ valid = false; break; }}\n", in0);
    out += std::format("{}  s.skipWhitespace();\n", in0);
    out += std::format("{}}}\n", in0);
  }

  // Close trailing optional if-blocks
  for (int i = baseInd; i > trailingBaseInd; i--)
    out += std::format("{}}}\n", indent(i - 1));

  // Close required field else-blocks
  for (int i = trailingBaseInd; i > 2; i--)
    out += std::format("{}}}\n", indent(i - 1));

  out += "  }\n"; // close "if non-empty"
  out += "  if (!s.expectChar(']')) return false;\n";

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
      std::string wireType = ci.WireIsUserType ? ci.WireUserTypeCppType : getWireTypeInfo(ci.MappedWireType).cppType;
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
  if (f.Type.IsScalar && f.Type.Scalar == EScalarType::String && f.Kind == EFieldKind::HasDefault)
    return "std::string_view";
  return "const " + type + " &";
}

static std::string flatSerializeParams(const CStructDef &s, const CContextAnalysis &ca,
    const std::unordered_set<std::string> &enumNames, const CCodegenOptions &opts)
{
  std::string params = "std::string &out";
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
  bool needMembers = s.GenerateFlags.Parse || s.GenerateFlags.ParseVerbose || s.GenerateFlags.Serialize || s.HasTaggedSchema;
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
      out += "  template<class Scanner> bool parseImpl(Scanner &s, Capture *capture);\n";
      out += std::format("  static bool resolve({} &out, const Capture &capture, {});\n", sn, ca.contextParams());
      if (hasBroadcastOverload)
        out += std::format("  static bool resolve({} &out, const Capture &capture, {});\n", sn, ca.broadcastContextParams());
    } else {
      out += "  bool parse(const char *buf, size_t bufSize);\n";
      out += "  template<class Scanner> bool parseImpl(Scanner &s);\n";
    }
  }

  if (s.GenerateFlags.ParseVerbose && !ca.hasContext()) {
    out += "  bool parseVerbose(const char *buf, size_t bufSize, ParseError &error);\n";
    out += "  template<class Scanner> bool parseVerboseImpl(Scanner &s);\n";
  }

  if (s.GenerateFlags.Serialize) {
    if (ca.hasContext()) {
      out += std::format("  void serialize(std::string &out, {}) const;\n", ca.contextParams());
      if (hasBroadcastOverload)
        out += std::format("  void serialize(std::string &out, {}) const;\n", ca.broadcastContextParams());
    } else
      out += "  void serialize(std::string &out) const;\n";
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

static void emitDerivedDirectResolveRecursive(std::string &out,
    const CFieldCaptureInfo &ci,
    const std::vector<CArrayDim> &dims, int dimIndex,
    const std::string &valueExpr,
    const std::string &captureExpr,
    int ind)
{
  std::string in = indent(ind);
  if (dimIndex >= (int)dims.size()) {
    std::string resolveName = "__" + ci.MappedTypeName + "Resolve";
    out += std::format("{}if (!{}({}, {}, {})) return false;\n",
                       in, resolveName, captureExpr, ci.DirectCtxParamName, valueExpr);
    return;
  }

  std::string idx = std::format("i{}_r_", dimIndex);
  out += std::format("{}for (size_t {} = 0; {} < {}.size(); {}++) {{\n", in, idx, idx, valueExpr, idx);
  emitDerivedDirectResolveRecursive(out, ci, dims, dimIndex + 1,
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
        case EFieldKind::HasDefault:
          emitDerivedDirectResolveRecursive(out, ci, dims, 0, "out." + cn, "capture." + ci.CaptureFieldName, 1);
          break;
        case EFieldKind::OptionalObject:
          out += std::format("  if (out.{}.has_value()) {{\n", cn);
          emitDerivedDirectResolveRecursive(out, ci, dims, 0,
                                           std::format("(*out.{})", cn),
                                           "capture." + ci.CaptureFieldName, 2);
          out += "  }\n";
          break;
        case EFieldKind::Array:
        case EFieldKind::FixedArray:
          emitDerivedDirectResolveRecursive(out, ci, dims, 0, "out." + cn, "capture." + ci.CaptureFieldName, 1);
          break;
        case EFieldKind::OptionalArray:
        case EFieldKind::OptionalFixedArray:
          out += std::format("  if (out.{}.has_value()) {{\n", cn);
          emitDerivedDirectResolveRecursive(out, ci, dims, 0,
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
        case EFieldKind::HasDefault:
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

static void emitDerivedDirectValueSerialize(CSerializeCodeBuilder &out,
    const CFieldCaptureInfo &fci,
    const std::string &valueExpr,
    int ind)
{
  std::string in = indent(ind);
  std::string serializeName = "__" + fci.MappedTypeName + "Serialize";
  out.appendRaw(std::format("{}{}(out, {}, {});\n",
                            in, serializeName, valueExpr, fci.DirectCtxParamName));
}

static void emitDerivedDirectSerializeRecursive(CSerializeCodeBuilder &out,
    const CFieldCaptureInfo &fci,
    const std::vector<CArrayDim> &dims, int dimIndex,
    const std::string &valueExpr,
    int ind)
{
  std::string in = indent(ind);
  if (dimIndex >= (int)dims.size()) {
    emitDerivedDirectValueSerialize(out, fci, valueExpr, ind);
    return;
  }

  std::string idx = std::format("i{}_s_", dimIndex);
  out.appendRaw(std::format("{}for (size_t {} = 0; {} < {}.size(); {}++) {{\n", in, idx, idx, valueExpr, idx));
  out.appendRaw(std::format("{}  if ({}) out += ',';\n", in, idx));
  emitDerivedDirectSerializeRecursive(out, fci, dims, dimIndex + 1,
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
  out.appendRaw(std::format("{}  if ({}) out += ',';\n", in, idx));
  emitNestedStructSerializeRecursive(out, ci, dims, dimIndex + 1,
                                     std::format("{}[{}]", valueExpr, idx),
                                     ctxIndexVar, ind + 1);
  out.appendRaw(std::format("{}}}\n", in));
}

// Serialize a context-free mapped field using its format function
static void emitDerivedInlineSerialize(CSerializeCodeBuilder &out, const CFieldDef &f,
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
      out.appendRaw(std::format("{}if ({}) out += ',';\n", keyIndent, commaVar));
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
      emitDerivedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, cn, ind);
      break;
    case EFieldKind::HasDefault: {
      static const std::unordered_set<std::string> noEnums;
      std::string def = cppDefault(f, noEnums, pascalCase);
      if (def.empty()) {
        emitKey(f.Name);
        emitDerivedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, cn, ind);
        break;
      }

      out.appendRaw(std::format("{}if ({} != {}) {{\n", in, cn, def));
      emitKey(f.Name, ind + 1);
      emitDerivedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, cn, ind + 1);
      out.appendRaw(std::format("{}}}\n", in));
      break;
    }
    case EFieldKind::OptionalObject:
      if (!fieldEmptyOutIsNull(f)) {
        out.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
        emitKey(f.Name, ind + 1);
        emitDerivedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("*{}", cn), ind + 1);
        out.appendRaw(std::format("{}}}\n", in));
        break;
      }
      emitKey(f.Name);
      out.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
      emitDerivedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("*{}", cn), ind + 1);
      out.appendRaw(std::format("{}}} else {{\n", in));
      out.writeLiteral(ind + 1, "null");
      out.appendRaw(std::format("{}}}\n", in));
      break;
    case EFieldKind::Array:
      emitKey(f.Name);
      out.writeLiteral(ind, "[");
      out.appendRaw(std::format("{}for (size_t i_ = 0; i_ < {}.size(); i_++) {{\n", in, cn));
      out.appendRaw(std::format("{}  if (i_) out += ',';\n", in));
      if (f.Type.InnerDims.empty()) {
        emitDerivedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("{}[i_]", cn), ind + 1);
      } else {
        emitDerivedInlineNestedArraySerialize(out, fci.MappedTypeName, fci.MappedWireType, f.Type.InnerDims, 0, std::format("{}[i_]", cn), ind + 1);
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
        out.appendRaw(std::format("{}    if (i_) out += ',';\n", in));
        if (f.Type.InnerDims.empty()) {
          emitDerivedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("(*{})[i_]", cn), ind + 2);
        } else {
          emitDerivedInlineNestedArraySerialize(out, fci.MappedTypeName, fci.MappedWireType, f.Type.InnerDims, 0, std::format("(*{})[i_]", cn), ind + 2);
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
      out.appendRaw(std::format("{}    if (i_) out += ',';\n", in));
      if (f.Type.InnerDims.empty()) {
        emitDerivedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("(*{})[i_]", cn), ind + 2);
      } else {
        emitDerivedInlineNestedArraySerialize(out, fci.MappedTypeName, fci.MappedWireType, f.Type.InnerDims, 0, std::format("(*{})[i_]", cn), ind + 2);
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
      out.appendRaw(std::format("{}  if (i_) out += ',';\n", in));
      if (f.Type.InnerDims.empty()) {
        emitDerivedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("{}[i_]", cn), ind + 1);
      } else {
        emitDerivedInlineNestedArraySerialize(out, fci.MappedTypeName, fci.MappedWireType, f.Type.InnerDims, 0, std::format("{}[i_]", cn), ind + 1);
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
        out.appendRaw(std::format("{}    if (i_) out += ',';\n", in));
        if (f.Type.InnerDims.empty()) {
          emitDerivedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("(*{})[i_]", cn), ind + 2);
        } else {
          emitDerivedInlineNestedArraySerialize(out, fci.MappedTypeName, fci.MappedWireType, f.Type.InnerDims, 0, std::format("(*{})[i_]", cn), ind + 2);
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
      out.appendRaw(std::format("{}    if (i_) out += ',';\n", in));
      if (f.Type.InnerDims.empty()) {
        emitDerivedInlineValueSerialize(out, fci.MappedTypeName, fci.MappedWireType, std::format("(*{})[i_]", cn), ind + 2);
      } else {
        emitDerivedInlineNestedArraySerialize(out, fci.MappedTypeName, fci.MappedWireType, f.Type.InnerDims, 0, std::format("(*{})[i_]", cn), ind + 2);
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
        writer.appendRaw(std::format("{}if (__needComma) out += ',';\n", keyIndent));
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
        case EFieldKind::HasDefault:
          emitKey(f.Name);
          emitDerivedDirectValueSerialize(writer, ci, cn, 1);
          break;
        case EFieldKind::OptionalObject:
          if (!fieldEmptyOutIsNull(f)) {
            writer.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
            emitKey(f.Name, 2);
            emitDerivedDirectValueSerialize(writer, ci, std::format("*{}", cn), 2);
            writer.appendRaw(std::format("{}}}\n", in));
            break;
          }
          emitKey(f.Name);
          writer.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
          emitDerivedDirectValueSerialize(writer, ci, std::format("*{}", cn), 2);
          writer.appendRaw(std::format("{}}} else {{\n", in));
          writer.writeLiteral(2, "null");
          writer.appendRaw(std::format("{}}}\n", in));
          break;
        case EFieldKind::Array:
        case EFieldKind::FixedArray:
          emitKey(f.Name);
          writer.writeLiteral(1, "[");
          emitDerivedDirectSerializeRecursive(writer, ci, dims, 0, cn, 1);
          writer.writeLiteral(1, "]");
          break;
        case EFieldKind::OptionalArray:
        case EFieldKind::OptionalFixedArray: {
          if (!fieldEmptyOutIsNull(f)) {
            writer.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
            emitKey(f.Name, 2);
            writer.writeLiteral(2, "[");
            emitDerivedDirectSerializeRecursive(writer, ci, dims, 0, std::format("(*{})", cn), 2);
            writer.writeLiteral(2, "]");
            writer.appendRaw(std::format("{}}}\n", in));
            break;
          }
          emitKey(f.Name);
          writer.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));
          writer.writeLiteral(2, "[");
          emitDerivedDirectSerializeRecursive(writer, ci, dims, 0, std::format("(*{})", cn), 2);
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
        case EFieldKind::HasDefault:
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
      emitDerivedInlineSerialize(writer, f, ci, 1, first, opts.PascalCaseFields, runtimeComma, "__needComma");
    else
      generateSerializeField(writer, f, enumNames, 1, first, opts.PascalCaseFields, runtimeComma, "__needComma");
  }
}

// Emit the value of a field in array_layout mode (no key, no conditional wrapping).
// For Required/HasDefault: scalar or struct. For Array/FixedArray: nested JSON array.
// For Variant: variant value. For Map: JSON object.
static void emitArrayLayoutValue(CSerializeCodeBuilder &writer, const CFieldDef &f,
    const std::string &cn,
    const std::unordered_set<std::string> &enumNames,
    int ind)
{
  std::string in = indent(ind);

  switch (f.Kind) {
    case EFieldKind::Required:
    case EFieldKind::HasDefault:
      if (isStructRef(f, enumNames))
        writer.appendRaw(std::format("{}{}.serialize(out);\n", in, cn));
      else
        emitSerializeValue(writer, f, cn, enumNames, ind);
      break;

    case EFieldKind::Array:
      writer.writeLiteral(ind, "[");
      writer.appendRaw(std::format("{}for (size_t i_ = 0; i_ < {}.size(); i_++) {{\n", in, cn));
      writer.appendRaw(std::format("{}  if (i_) out += ',';\n", in));
      if (f.Type.InnerDims.empty())
        emitSerializeArrayElem(writer, f, std::format("{}[i_]", cn), enumNames, ind + 1);
      else
        emitNestedArraySerialize(writer, f, f.Type.InnerDims, 0, std::format("{}[i_]", cn), enumNames, ind + 1);
      writer.appendRaw(std::format("{}}}\n", in));
      writer.writeLiteral(ind, "]");
      break;

    case EFieldKind::FixedArray:
      writer.writeLiteral(ind, "[");
      writer.appendRaw(std::format("{}for (size_t i_ = 0; i_ < {}.size(); i_++) {{\n", in, cn));
      writer.appendRaw(std::format("{}  if (i_) out += ',';\n", in));
      if (f.Type.InnerDims.empty())
        emitSerializeArrayElem(writer, f, std::format("{}[i_]", cn), enumNames, ind + 1);
      else
        emitNestedArraySerialize(writer, f, f.Type.InnerDims, 0, std::format("{}[i_]", cn), enumNames, ind + 1);
      writer.appendRaw(std::format("{}}}\n", in));
      writer.writeLiteral(ind, "]");
      break;

    case EFieldKind::Variant:
      emitVariantSerialize(writer, f, cn, enumNames, ind);
      break;

    case EFieldKind::Map:
      writer.writeLiteral(ind, "{");
      writer.appendRaw(std::format("{}{{ bool _first = true;\n", in));
      writer.appendRaw(std::format("{}for (auto &[_k, _v] : {}) {{\n", in, cn));
      writer.appendRaw(std::format("{}  if (!_first) out += ',';\n", in));
      writer.appendRaw(std::format("{}  _first = false;\n", in));
      writer.appendRaw(std::format("{}  jsonWriteString(out, _k);\n", in));
      writer.appendRaw(std::format("{}  out += ':';\n", in));
      if (isStructRef(f, enumNames))
        writer.appendRaw(std::format("{}  _v.serialize(out);\n", in));
      else {
        CFieldDef elemField = f;
        elemField.Kind = EFieldKind::Required;
        emitSerializeValue(writer, elemField, "_v", enumNames, ind + 1);
      }
      writer.appendRaw(std::format("{}}}\n", in));
      writer.appendRaw(std::format("{}}}\n", in));
      writer.writeLiteral(ind, "}");
      break;

    default:
      break;
  }
}

// Emit the value of an optional field in array_layout mode:
// if present → emit value, if absent → emit null
static void emitArrayLayoutOptionalValue(CSerializeCodeBuilder &writer, const CFieldDef &f,
    const std::string &cn,
    const std::unordered_set<std::string> &enumNames,
    int ind)
{
  std::string in = indent(ind);
  writer.appendRaw(std::format("{}if ({}.has_value()) {{\n", in, cn));

  // Emit the inner value
  CFieldDef innerField = f;
  std::string innerCn;
  switch (f.Kind) {
    case EFieldKind::OptionalObject:  innerField.Kind = EFieldKind::Required; innerCn = std::format("(*{})", cn); break;
    case EFieldKind::OptionalArray:   innerField.Kind = EFieldKind::Array; innerCn = std::format("(*{})", cn); break;
    case EFieldKind::OptionalFixedArray: innerField.Kind = EFieldKind::FixedArray; innerCn = std::format("(*{})", cn); break;
    case EFieldKind::OptionalVariant: innerField.Kind = EFieldKind::Variant; innerCn = std::format("(*{})", cn); break;
    case EFieldKind::OptionalMap:     innerField.Kind = EFieldKind::Map; innerCn = std::format("(*{})", cn); break;
    default: innerCn = cn; break;
  }
  emitArrayLayoutValue(writer, innerField, innerCn, enumNames, ind + 1);

  writer.appendRaw(std::format("{}}} else {{\n", in));
  writer.writeLiteral(ind + 1, "null");
  writer.appendRaw(std::format("{}}}\n", in));
}

static void generateArrayLayoutSerializeBody(CSerializeCodeBuilder &writer, const CStructDef &s,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts)
{
  if (s.Fields.empty())
    return;

  // Find boundary: required fields [0..firstTrailing), optional/default [firstTrailing..N)
  size_t firstTrailing = s.Fields.size();
  for (size_t i = 0; i < s.Fields.size(); i++) {
    if (s.Fields[i].Kind == EFieldKind::HasDefault || isOptionalField(s.Fields[i])) {
      firstTrailing = i;
      break;
    }
  }

  size_t totalFields = s.Fields.size();
  bool hasTrailing = (firstTrailing < totalFields);

  // Compute __lastField: index+1 of the last field that needs serializing.
  // If an empty optional with null_in=deny is encountered, stop — we can't
  // emit null (the parser would reject it), so the array is truncated there.
  if (hasTrailing) {
    // Check if any trailing optional field has a barrier (null_in=deny)
    bool anyBarrier = false;
    for (size_t i = firstTrailing; i < totalFields; i++) {
      if (isOptionalField(s.Fields[i]) && !fieldAllowsNullInput(s.Fields[i])) {
        anyBarrier = true;
        break;
      }
    }

    writer.appendRaw(std::format("{}size_t __lastField = {};\n", indent(1), firstTrailing));
    int ind = anyBarrier ? 2 : 1;
    if (anyBarrier)
      writer.appendRaw(std::format("{}do {{\n", indent(1)));

    for (size_t i = firstTrailing; i < totalFields; i++) {
      auto &f = s.Fields[i];
      std::string cn = fieldCppName(f.Name, opts.PascalCaseFields);
      if (isOptionalField(f)) {
        writer.appendRaw(std::format("{}if ({}.has_value()) __lastField = {};\n", indent(ind), cn, i + 1));
        if (!fieldAllowsNullInput(f))
          writer.appendRaw(std::format("{}else break;\n", indent(ind)));
      } else if (f.Kind == EFieldKind::HasDefault) {
        std::string defaultVal = cppDefault(f, enumNames, opts.PascalCaseFields);
        writer.appendRaw(std::format("{}if ({} != {}) __lastField = {};\n", indent(ind), cn, defaultVal, i + 1));
      }
    }

    if (anyBarrier)
      writer.appendRaw(std::format("{}}} while (false);\n", indent(1)));
  }

  // Serialize required fields (always present)
  for (size_t i = 0; i < firstTrailing; i++) {
    auto &f = s.Fields[i];
    std::string cn = fieldCppName(f.Name, opts.PascalCaseFields);
    if (i > 0)
      writer.writeLiteral(1, ",");
    emitArrayLayoutValue(writer, f, cn, enumNames, 1);
  }

  // Serialize trailing optional/default fields — only up to __lastField
  for (size_t i = firstTrailing; i < totalFields; i++) {
    auto &f = s.Fields[i];
    std::string cn = fieldCppName(f.Name, opts.PascalCaseFields);
    std::string in = indent(1);

    writer.appendRaw(std::format("{}if (__lastField > {}) {{\n", in, i));

    if (i > 0)
      writer.writeLiteral(2, ",");

    if (isOptionalField(f)) {
      emitArrayLayoutOptionalValue(writer, f, cn, enumNames, 2);
    } else {
      // HasDefault: always has a value in C++
      emitArrayLayoutValue(writer, f, cn, enumNames, 2);
    }

    writer.appendRaw(std::format("{}}}\n", in));
  }
}

static void generateSerializeBody(CSerializeCodeBuilder &writer, const CStructDef &s,
    const CContextAnalysis &ca,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts)
{
  if (s.TopLevel) {
    auto &f = s.Fields[0];
    std::string cn = fieldCppName(f.Name, opts.PascalCaseFields);
    emitArrayLayoutValue(writer, f, cn, enumNames, 1);
  } else if (ca.hasContext()) {
    generateContextSerializeBody(writer, s, ca, enumNames, opts);
  } else if (s.ArrayLayout) {
    generateArrayLayoutSerializeBody(writer, s, enumNames, opts);
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
        emitDerivedInlineSerialize(writer, f, fci, 1, first, opts.PascalCaseFields, runtimeComma, "__needComma");
      } else {
        generateSerializeField(writer, f, enumNames, 1, first, opts.PascalCaseFields, runtimeComma, "__needComma");
      }
    }
  }
}

static void generateStructImpl(std::string &out, const CStructDef &s,
    const std::unordered_set<std::string> &enumNames,
    const CCodegenOptions &opts,
    const CContextAnalysis &ca,
    bool anyComments)
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
    // Count required fields
    auto isRequired = [](const CFieldDef &f) {
      return f.Kind == EFieldKind::Required || f.Kind == EFieldKind::Array ||
             f.Kind == EFieldKind::FixedArray || f.Kind == EFieldKind::Variant ||
             f.Kind == EFieldKind::Map || fieldRequiresPresence(f);
    };
    for (auto &f : s.Fields)
      if (isRequired(f))
        requiredCount++;

    // Assign required field bits
    int bitIndex = 0;
    for (size_t i = 0; i < s.Fields.size(); i++)
      if (isRequired(s.Fields[i]))
        fieldBitMap[i] = bitIndex++;

    // Perfect hash (not needed for array_layout or toplevel)
    if (!s.ArrayLayout && !s.TopLevel) {
      for (auto &f : s.Fields)
        fieldNames.push_back(f.Name);

      ph = findPerfectHash(fieldNames);
      if (!ph) {
        fprintf(stderr, "warning: cannot find perfect hash for struct '%s', using mod=%zu\n",
                s.Name.c_str(), fieldNames.size() * 4);
        ph = CPerfectHash{0, 31, (uint32_t)(fieldNames.size() * 4)};
      }

      // Build hash -> field index map
      for (size_t i = 0; i < s.Fields.size(); i++) {
        uint32_t h = computeHash(s.Fields[i].Name.c_str(), s.Fields[i].Name.size(),
                                  ph->Seed, ph->Mult, ph->Mod);
        hashToIndex[h] = i;
      }
    }
  }

  // --- Parse ---
  const char *scannerType = s.CommentsEnabled ? "JsonScannerComments" : "JsonScanner";
  const char *verboseScannerType = s.CommentsEnabled ? "VerboseJsonScannerComments" : "VerboseJsonScanner";

  if (s.GenerateFlags.Parse) {
    if (hasCtx) {
      out += std::format("bool {}::parse(const char *buf, size_t bufSize, Capture &capture) {{\n", sn);
      out += std::format("  {} s{{buf, buf + bufSize}};\n", scannerType);
      out += "  if (!parseImpl(s, &capture)) return false;\n";
      out += "  s.skipWhitespace();\n";
      out += "  return s.p == s.end;\n";
      out += "}\n\n";

      out += std::format("template<class Scanner>\nbool {}::parseImpl(Scanner &s, Capture *capture) {{\n", sn);
      if (s.TopLevel)
        generateTopLevelParseBody(out, s, enumNames, opts, &ca.FieldCapture);
      else
        generateParseBody(out, s, enumNames, opts, *ph, hashToIndex, fieldBitMap, requiredCount, &ca.FieldCapture);
      out += "}\n";
      out += std::format("template bool {}::parseImpl(JsonScanner &, Capture *);\n", sn);
      if (anyComments)
        out += std::format("template bool {}::parseImpl(JsonScannerComments &, Capture *);\n", sn);
      out += "\n";
    } else {
      out += std::format("bool {}::parse(const char *buf, size_t bufSize) {{\n", sn);
      out += std::format("  {} s{{buf, buf + bufSize}};\n", scannerType);
      out += "  if (!parseImpl(s)) return false;\n";
      out += "  s.skipWhitespace();\n";
      out += "  return s.p == s.end;\n";
      out += "}\n\n";

      out += std::format("template<class Scanner>\nbool {}::parseImpl(Scanner &s) {{\n", sn);
      if (s.TopLevel)
        generateTopLevelParseBody(out, s, enumNames, opts, &ca.FieldCapture);
      else if (s.ArrayLayout)
        generateArrayLayoutParseBody(out, s, enumNames, opts, fieldBitMap, requiredCount);
      else
        generateParseBody(out, s, enumNames, opts, *ph, hashToIndex, fieldBitMap, requiredCount, &ca.FieldCapture);
      out += "}\n";
      out += std::format("template bool {}::parseImpl(JsonScanner &);\n", sn);
      if (anyComments)
        out += std::format("template bool {}::parseImpl(JsonScannerComments &);\n", sn);
      out += "\n";
    }
  }

  // --- Verbose parse ---
  if (s.GenerateFlags.ParseVerbose && !hasCtx) {
    out += std::format("bool {}::parseVerbose(const char *buf, size_t bufSize, ParseError &error) {{\n", sn);
    out += std::format("  {} s{{buf, buf + bufSize, buf, &error}};\n", verboseScannerType);
    out += "  if (!parseVerboseImpl(s)) return false;\n";
    out += "  s.skipWhitespace();\n";
    out += "  if (s.p != s.end) {\n";
    out += "    s.setError(\"trailing characters after JSON value\");\n";
    out += "    return false;\n";
    out += "  }\n";
    out += "  return true;\n";
    out += "}\n\n";

    out += std::format("template<class Scanner>\nbool {}::parseVerboseImpl(Scanner &s) {{\n", sn);

    if (s.TopLevel) {
      // Toplevel verbose: reuse verbose field codegen
      auto &f = s.Fields[0];
      generateVerboseParseField(out, f, enumNames, -1, 1, opts.PascalCaseFields);
    } else if (s.ArrayLayout) {
      // Array-layout verbose parse
      out += "  if (!s.expectChar('[')) return false;\n";
      if (requiredCount > 0)
        out += "  uint64_t found = 0;\n";
      out += "  s.skipWhitespace();\n";

      // Find boundary: required [0..firstTrailing), optional/default [firstTrailing..N)
      size_t firstTrailing = s.Fields.size();
      for (size_t i = 0; i < s.Fields.size(); i++) {
        if (s.Fields[i].Kind == EFieldKind::HasDefault || isOptionalField(s.Fields[i])) {
          firstTrailing = i;
          break;
        }
      }

      out += "  if (s.p < s.end && *s.p != ']') {\n";

      int baseInd = 2;
      for (size_t i = 0; i < firstTrailing; i++) {
        auto &f = s.Fields[i];
        int foundBit = -1;
        auto bitIt = fieldBitMap.find(i);
        if (bitIt != fieldBitMap.end())
          foundBit = bitIt->second;

        if (i > 0) {
          std::string in0 = indent(baseInd);
          out += std::format("{}if (!s.expectChar(',')) return false;\n", in0);
          out += std::format("{}s.skipWhitespace();\n", in0);
        }

        std::string verboseFieldCode;
        generateVerboseParseField(verboseFieldCode, f, enumNames, foundBit, baseInd, opts.PascalCaseFields);
        out += verboseFieldCode;
      }

      int trailingBaseInd = baseInd;
      for (size_t i = firstTrailing; i < s.Fields.size(); i++) {
        auto &f = s.Fields[i];
        int foundBit = -1;
        auto bitIt = fieldBitMap.find(i);
        if (bitIt != fieldBitMap.end())
          foundBit = bitIt->second;

        std::string in0 = indent(baseInd);
        if (i == 0) {
          // First element — no comma needed
        } else {
          out += std::format("{}s.skipWhitespace();\n", in0);
          out += std::format("{}if (s.p < s.end && *s.p == ',') {{\n", in0);
          out += std::format("{}s.p++; s.skipWhitespace();\n", indent(baseInd + 1));
          baseInd++;
        }

        std::string verboseFieldCode;
        generateVerboseParseField(verboseFieldCode, f, enumNames, foundBit, baseInd, opts.PascalCaseFields);
        out += verboseFieldCode;
      }

      if (s.SkipUnknownFields) {
        std::string in0 = indent(baseInd);
        out += std::format("{}s.skipWhitespace();\n", in0);
        out += std::format("{}while (s.p < s.end && *s.p == ',') {{\n", in0);
        out += std::format("{}  s.p++; s.skipWhitespace();\n", in0);
        out += std::format("{}  if (!s.skipValue()) return false;\n", in0);
        out += std::format("{}  s.skipWhitespace();\n", in0);
        out += std::format("{}}}\n", in0);
      }

      for (int i = baseInd; i > trailingBaseInd; i--)
        out += std::format("{}}}\n", indent(i - 1));

      out += "  }\n";
      out += "  if (!s.expectChar(']')) return false;\n";

      if (requiredCount > 0) {
        uint64_t mask = ((uint64_t)1 << requiredCount) - 1;
        out += std::format("  if ((found & 0x{:x}) != 0x{:x}) {{\n", mask, mask);
        for (size_t i = 0; i < s.Fields.size(); i++) {
          auto bitIt2 = fieldBitMap.find(i);
          if (bitIt2 != fieldBitMap.end()) {
            out += std::format("    if (!(found & ((uint64_t)1 << {}))) "
                               "{{ s.setError(\"missing element at index {} (field '{}')\"); return false; }}\n",
                               bitIt2->second, i, s.Fields[i].Name);
          }
        }
        out += "  }\n";
      }
    } else {
      // Normal object-based verbose parse
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
        if (s.SkipUnknownFields) {
          out += "      if (!s.skipValue()) return false;\n";
        } else {
          out += "      s.setError(\"unknown field '\" + std::string(key, keyLen) + \"'\");\n";
          out += "      return false;\n";
        }
      } else {
        out += "      uint32_t keyHash;\n";
        out += std::format("      if (!s.readStringHash(key, keyLen, keyHash, {}, {})) return false;\n",
                           ph->Seed, ph->Mult);
        out += "      if (!s.expectChar(':')) return false;\n";
        out += "      s.skipWhitespace();\n";
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

          if (s.SkipUnknownFields) {
            out += "          } else { if (!s.skipValue()) return false; }\n";
          } else {
            out += "          } else { s.setError(\"unknown field '\" + std::string(key, keyLen) + \"'\"); return false; }\n";
          }
          out += "          break;\n";
        }

        out += "        default:\n";
        if (s.SkipUnknownFields) {
          out += "          if (!s.skipValue()) return false;\n";
        } else {
          out += "          s.setError(\"unknown field '\" + std::string(key, keyLen) + \"'\");\n";
          out += "          return false;\n";
        }
        out += "      }\n";
      }

      out += "      s.skipWhitespace();\n";
      out += "      if (s.p < s.end && *s.p == ',') { s.p++; continue; }\n";
      out += "      break;\n";
      out += "    }\n";
      out += "  }\n";
      out += "  if (!s.expectCharNoWs('}')) return false;\n";

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
    }

    out += "  return true;\n";
    out += "}\n";
    out += std::format("template bool {}::parseVerboseImpl(VerboseJsonScanner &);\n", sn);
    if (anyComments)
      out += std::format("template bool {}::parseVerboseImpl(VerboseJsonScannerComments &);\n", sn);
    out += "\n";
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
      out += std::format("void {}::serialize(std::string &out, {}) const {{\n", sn, ctxParams);
    } else {
      out += std::format("void {}::serialize(std::string &out) const {{\n", sn);
    }
    {
      CSerializeCodeBuilder writer(out);
      if (s.TopLevel) {
        generateSerializeBody(writer, s, ca, enumNames, opts);
      } else {
        const char *open = s.ArrayLayout ? "[" : "{";
        const char *close = s.ArrayLayout ? "]" : "}";
        writer.writeLiteral(1, open);
        generateSerializeBody(writer, s, ca, enumNames, opts);
        writer.writeLiteral(1, close);
      }
      writer.flush();
    }
    out += "}\n\n";

    if (hasBroadcastOverload) {
      CContextAnalysis broadcast = ca.broadcastVariant();
      out += std::format("void {}::serialize(std::string &out, {}) const {{\n", sn, broadcast.broadcastContextParams());
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
      const char *open = s.ArrayLayout ? "[" : "{";
      const char *close = s.ArrayLayout ? "]" : "}";
      CSerializeCodeBuilder writer(out);
      writer.writeLiteral(1, open);
      generateSerializeBody(writer, s, ca, enumNames, opts);
      writer.writeLiteral(1, close);
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

  // Build derived type index
  std::unordered_map<std::string, const CDerivedTypeDef *> derivedTypeIndex;
  for (auto &dt : file.DerivedTypes)
    derivedTypeIndex[dt.Name] = &dt;

  // Analyze context for all structs (topological order — children first)
  std::unordered_map<std::string, CContextAnalysis> contextAnalysis;
  for (auto &s : file.Structs) {
    if (s.IsMixin) continue;
    contextAnalysis[s.Name] = analyzeContext(s, opts.PascalCaseFields, derivedTypeIndex, contextAnalysis);
  }

  // Check features
  bool hasTaggedSchema = false;
  bool hasChrono = false;
  bool anyComments = false;
  bool anyFlatSerialize = false;
  bool hasFixedArray = false;
  bool hasVariant = false;
  bool hasMap = false;
  CJsonHelperUsage jsonHelperUsage;
  CJsonReadUsage jsonReadUsage;
  for (auto &s : file.Structs) {
    if (!s.IsMixin && !s.IsImported) {
      if (s.HasTaggedSchema) hasTaggedSchema = true;
      if (s.CommentsEnabled) anyComments = true;
      if (s.GenerateFlags.SerializeFlat) anyFlatSerialize = true;
      if (s.GenerateFlags.Serialize || s.GenerateFlags.SerializeFlat) {
        auto &ca = contextAnalysis[s.Name];
        for (size_t i = 0; i < s.Fields.size(); i++)
          collectSerializeHelpersForField(jsonHelperUsage, s.Fields[i], ca.FieldCapture[i], enumNames);
      }
      if (s.GenerateFlags.Parse || s.GenerateFlags.ParseVerbose) {
        for (auto &f : s.Fields)
          collectParseReadUsageForField(jsonReadUsage, f, enumNames);
      }
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
        if (f.Kind == EFieldKind::Map || f.Kind == EFieldKind::OptionalMap)
          hasMap = true;
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
  if (hasMap)
    header += "#include <unordered_map>\n";
  if (anyFlatSerialize)
    header += "#include <string_view>\n";
  if (hasTaggedSchema)
    header += "#include \"idltool/schema.h\"\n";

  // Usertype headers
  for (auto &ut : file.UserTypes) {
    if (!ut.IsImported)
      header += std::format("#include \"{}\"\n", ut.IncludePath);
  }
  // Derived type headers
  for (auto &dt : file.DerivedTypes) {
    if (!dt.IsImported)
      header += std::format("#include \"{}\"\n", dt.IncludePath);
  }
  header += "\n";

  // Include directives
  for (auto &inc : file.Includes)
    header += std::format("#include \"{}.h\"\n", inc.Path);
  if (!file.Includes.empty())
    header += "\n";

  // Scanner and forward declarations
  header += "#include \"idltool/jsonScanner.h\"\n";
  header += "#include <string>\n\n";

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
  source += "#include <string>\n";
  source += "#include <cstring>\n";
  source += "#include <cstdlib>\n";
  source += "#include <cinttypes>\n";
  if (jsonReadUsage.ReadString)
    source += "#include \"idltool/jsonReadString.h\"\n";
  if (jsonReadUsage.ReadInt)
    source += "#include \"idltool/jsonReadInt.h\"\n";
  if (jsonReadUsage.ReadUInt)
    source += "#include \"idltool/jsonReadUInt.h\"\n";
  if (jsonReadUsage.ReadDouble)
    source += "#include \"idltool/jsonReadDouble.h\"\n";
  if (jsonReadUsage.ReadBool)
    source += "#include \"idltool/jsonReadBool.h\"\n";
  if (jsonHelperUsage.WriteString)
    source += "#include \"idltool/jsonWriteString.h\"\n";
  if (jsonHelperUsage.WriteInt)
    source += "#include \"idltool/jsonWriteInt.h\"\n";
  if (jsonHelperUsage.WriteUInt)
    source += "#include \"idltool/jsonWriteUInt.h\"\n";
  if (jsonHelperUsage.WriteDouble) {
    source += "#include \"idltool/jsonWriteDouble.h\"\n";
    source += "#include <cmath>\n";
  }
  if (jsonHelperUsage.WriteBool)
    source += "#include \"idltool/jsonWriteBool.h\"\n";
  if (jsonReadUsage.ReadHexData)
    source += "#include \"idltool/jsonReadHexData.h\"\n";
  if (jsonHelperUsage.WriteHexData)
    source += "#include \"idltool/jsonWriteHexData.h\"\n";
  source += "\n";

  generateEnumDefinitions(source, file, opts.PascalCaseFields);

  for (auto &s : file.Structs) {
    if (s.IsMixin || s.IsImported) continue;
    generateStructImpl(source, s, enumNames, opts, contextAnalysis[s.Name], anyComments);
  }

  return result;
}
