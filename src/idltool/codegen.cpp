#include "codegen.h"
#include "codegenCommon.h"
#include "codegenVerbose.h"
#include <cstdio>
#include <cstdint>
#include <format>
#include <unordered_set>
#include <unordered_map>

// --- Minimal parse generation ---

static void generateParseScalar(std::string &out, const CFieldDef &f, const std::unordered_set<std::string> &enumNames, int foundBit, int ind, bool pascalCase, int externCaptureIdx = -1)
{
  std::string in = indent(ind);
  std::string cn = fieldCppName(f.Name, pascalCase);

  // Extern field: skip value (non-capture mode) or read into _capture (capture mode)
  if (isExternField(f)) {
    if (externCaptureIdx >= 0) {
      out += std::format("{}if (_capture) {{\n", in);
      out += std::format("{}  if (!s.readStringValue(_capture[{}])) valid = false;\n", in, externCaptureIdx);
      if (foundBit >= 0)
        out += std::format("{}  else found |= (uint64_t)1 << {};\n", in, foundBit);
      out += std::format("{}}} else {{\n", in);
      out += std::format("{}  if (!s.skipValue()) valid = false;\n", in);
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

  // Chrono types: read as int64_t then construct
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

// captureIdx: >= 0 for extern/deferred fields, index into _capture array
// captureIsDeferred: true for context-threaded struct fields (capture raw JSON)
static void generateParseField(std::string &out, const CFieldDef &f, const std::unordered_set<std::string> &enumNames, int foundBit, int ind, bool pascalCase, int captureIdx = -1, bool captureIsDeferred = false)
{
  std::string in = indent(ind);
  std::string cn = fieldCppName(f.Name, pascalCase);

  switch (f.Kind) {
    case EFieldKind::Required:
    case EFieldKind::Optional: {
      if (isStructRef(f, enumNames)) {
        out += std::format("{}if (!{}.parseImpl(s)) valid = false;\n", in, cn);
        if (foundBit >= 0)
          out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
      } else {
        generateParseScalar(out, f, enumNames, foundBit, ind, pascalCase, isExternField(f) ? captureIdx : -1);
      }
      break;
    }

    case EFieldKind::OptionalObject: {
      if (captureIsDeferred && captureIdx >= 0) {
        // Context-threaded struct: capture raw JSON when _capture is non-null, else parse inline
        out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
        out += std::format("{}else if (_capture) {{\n", in);
        out += std::format("{}  s.skipWhitespace();\n", in);
        out += std::format("{}  const char *_start = s.p;\n", in);
        out += std::format("{}  if (!s.skipValue()) valid = false;\n", in);
        out += std::format("{}  else _capture[{}].assign(_start, s.p - _start);\n", in, captureIdx);
        out += std::format("{}}}\n", in);
        // Fallback: parse inline when _capture is null (extern sub-fields will be skipped)
        if (isStructRef(f, enumNames))
          out += std::format("{}else if (!{}.emplace().parseImpl(s)) valid = false;\n", in, cn);
        else
          out += std::format("{}else {{ if (!s.skipValue()) valid = false; }}\n", in);
      } else {
        out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
        if (isStructRef(f, enumNames)) {
          out += std::format("{}else if (!{}.emplace().parseImpl(s)) valid = false;\n", in, cn);
        } else {
          out += std::format("{}else {{\n", in);
          out += std::format("{}  {}.emplace();\n", in, cn);
          CFieldDef tmp = f;
          tmp.Name = std::format("(*{})", cn);
          tmp.Kind = EFieldKind::Required;
          generateParseScalar(out, tmp, enumNames, -1, ind + 1, false);
          out += std::format("{}}}\n", in);
        }
      }
      break;
    }

    case EFieldKind::Array: {
      out += std::format("{}if (!s.expectChar('[')) {{ valid = false; break; }}\n", in);
      out += std::format("{}s.skipWhitespace();\n", in);
      out += std::format("{}if (s.p < s.end && *s.p != ']') {{\n", in);
      out += std::format("{}  for (;;) {{\n", in);

      if (isStructRef(f, enumNames)) {
        out += std::format("{}    if (!{}.emplace_back().parseImpl(s)) {{ valid = false; break; }}\n", in, cn);
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

    case EFieldKind::OptionalArray: {
      out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      out += std::format("{}else {{\n", in);
      out += std::format("{}  {}.emplace();\n", in, cn);
      CFieldDef tmp = f;
      tmp.Name = std::format("(*{})", cn);
      tmp.Kind = EFieldKind::Array;
      generateParseField(out, tmp, enumNames, -1, ind + 1, false);
      out += std::format("{}}}\n", in);
      break;
    }
  }
}

// --- Struct declaration (header) ---

// Per-struct extern type analysis
struct CExternAnalysis {
  bool HasExternDirect = false;   // struct has extern-type fields directly
  bool HasExternNested = false;   // struct has struct-type fields with context(X)
  bool NeedsExternalContext = false; // has extern fields without self-contained context resolution
  bool SelfContainedContext = false; // all extern fields have context within this struct

  // Direct extern fields: {fieldName, contextField (may be empty)}
  struct ExternFieldInfo {
    std::string FieldName;
    std::string ContextField;
  };
  std::vector<ExternFieldInfo> ExternFields;

  // Nested struct fields that have context threading
  struct NestedContextInfo {
    std::string FieldName;  // the struct field
    std::string ContextField; // the context field in this struct
    bool IsOptional;
  };
  std::vector<NestedContextInfo> NestedContextFields;
};

static CExternAnalysis analyzeExternFields(const CStructDef &s, bool pascalCase)
{
  CExternAnalysis a;

  for (auto &f : s.Fields) {
    if (f.Type.IsExtern) {
      a.HasExternDirect = true;
      a.ExternFields.push_back({fieldCppName(f.Name, pascalCase), f.ContextField});
    }
    if (!f.ContextField.empty() && !f.Type.IsExtern && !f.Type.IsScalar && !f.Type.RefName.empty()) {
      // Struct field with context() annotation = context threading
      a.HasExternNested = true;
      bool isOpt = (f.Kind == EFieldKind::OptionalObject);
      a.NestedContextFields.push_back({fieldCppName(f.Name, pascalCase), fieldCppName(f.ContextField, pascalCase), isOpt});
    }
  }

  // Determine context resolution strategy
  if (a.HasExternDirect) {
    // Check if all extern fields have context from a field in the same struct
    bool allHaveContext = true;
    for (auto &ef : a.ExternFields) {
      if (ef.ContextField.empty()) {
        allHaveContext = false;
        break;
      }
    }
    if (allHaveContext)
      a.SelfContainedContext = true;
    else
      a.NeedsExternalContext = true;
  }

  return a;
}

static void generateStructDecl(std::string &out, const CStructDef &s, const std::unordered_set<std::string> &enumNames, const CCodegenOptions &opts)
{
  std::string sn = opts.StructPrefix + s.Name;
  CExternAnalysis ea = analyzeExternFields(s, opts.PascalCaseFields);
  bool hasExtern = ea.HasExternDirect || ea.HasExternNested;

  // Compute capture array size: direct extern fields + context-threaded nested fields
  int captureCount = (int)ea.ExternFields.size() + (int)ea.NestedContextFields.size();

  out += std::format("struct {} {{\n", sn);

  // Fields (no _raw_ members)
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

  // Method declarations
  out += "  bool parse(const char *buf, size_t bufSize);\n";
  out += "  bool parseImpl(JsonScanner &s);\n";
  if (hasExtern) {
    out += "  bool parseImpl(JsonScanner &s, std::string *_capture);\n";
    out += std::format("  bool parseWithCapture(const char *buf, size_t bufSize, std::string (&_capture)[{}]);\n", captureCount);
  }
  if (opts.VerboseMode) {
    out += "  bool parseVerbose(const char *buf, size_t bufSize, ParseError &error);\n";
    out += "  bool parseVerboseImpl(VerboseJsonScanner &s);\n";
  }
  out += "  std::string serialize() const;\n";
  out += "  void serializeImpl(std::string &out) const;\n";
  if (hasExtern)
    out += "  void serializeImpl(std::string &out, const std::string *_capture) const;\n";

  // Template parse/serialize for extern field resolution
  if (hasExtern) {
    // Template parse: resolver converts raw strings to typed values
    if (ea.NeedsExternalContext) {
      // Direct extern fields, needs external context: resolver(raw, out) → bool
      out += "\n  template<typename F>\n";
      out += "  bool parse(const char *buf, size_t bufSize, F &&_resolver) {\n";
      out += std::format("    std::string _capture[{}];\n", captureCount);
      out += "    if (!parseWithCapture(buf, bufSize, _capture)) return false;\n";
      int idx = 0;
      for (auto &ef : ea.ExternFields) {
        out += std::format("    if (!_resolver(_capture[{}], {})) return false;\n", idx, ef.FieldName);
        idx++;
      }
      out += "    return true;\n";
      out += "  }\n";
    }

    if (ea.SelfContainedContext || ea.HasExternNested) {
      // Self-contained or context-threaded: resolver(ctx, raw, out) → bool
      out += "\n  template<typename F>\n";
      out += "  bool parse(const char *buf, size_t bufSize, F &&_resolver) {\n";
      out += std::format("    std::string _capture[{}];\n", captureCount);
      out += "    if (!parseWithCapture(buf, bufSize, _capture)) return false;\n";
      // Direct extern fields with self-contained context
      int idx = 0;
      for (auto &ef : ea.ExternFields) {
        if (!ef.ContextField.empty())
          out += std::format("    if (!_resolver({}, _capture[{}], {})) return false;\n",
                             ef.ContextField, idx, ef.FieldName);
        idx++;
      }
      // Context-threaded nested struct fields
      for (auto &nc : ea.NestedContextFields) {
        out += std::format("    if (!_capture[{}].empty()) {{\n", idx);
        out += std::format("      {}.emplace();\n", nc.FieldName);
        out += std::format("      if (!{}->parse(_capture[{}].c_str(), _capture[{}].size(),\n",
                           nc.FieldName, idx, idx);
        out += std::format("        [&_resolver, this](const std::string &_raw, auto &_out) {{\n");
        out += std::format("          return _resolver({}, _raw, _out);\n", nc.ContextField);
        out += "        })) return false;\n";
        out += "    }\n";
        idx++;
      }
      out += "    return true;\n";
      out += "  }\n";
    }

    // Template serialize: formatter converts typed values to raw strings
    if (ea.NeedsExternalContext) {
      // Direct extern fields: formatter(value) → string
      out += "\n  template<typename F>\n";
      out += "  std::string serialize(F &&_formatter) const {\n";
      out += std::format("    const std::string _capture[{}] = {{\n", captureCount);
      for (auto &ef : ea.ExternFields)
        out += std::format("      _formatter({}),\n", ef.FieldName);
      out += "    };\n";
      out += "    std::string out;\n";
      out += "    serializeImpl(out, _capture);\n";
      out += "    return out;\n";
      out += "  }\n";
    }

    if (ea.SelfContainedContext || ea.HasExternNested) {
      // Context-threaded: formatter(ctx, value) → string
      out += "\n  template<typename F>\n";
      out += "  std::string serialize(F &&_formatter) const {\n";
      out += std::format("    std::string _capture[{}];\n", captureCount);
      int idx = 0;
      for (auto &ef : ea.ExternFields) {
        if (!ef.ContextField.empty())
          out += std::format("    _capture[{}] = _formatter({}, {});\n", idx, ef.ContextField, ef.FieldName);
        idx++;
      }
      for (auto &nc : ea.NestedContextFields) {
        if (nc.IsOptional)
          out += std::format("    if ({}.has_value())\n  ", nc.FieldName);
        out += std::format("    _capture[{}] = {}serialize(\n", idx, nc.IsOptional ? nc.FieldName + "->" : nc.FieldName + ".");
        out += std::format("      [&_formatter, this](const auto &_val) -> std::string {{\n");
        out += std::format("        return _formatter({}, _val);\n", nc.ContextField);
        out += "      });\n";
        idx++;
      }
      out += "    std::string out;\n";
      out += "    serializeImpl(out, _capture);\n";
      out += "    return out;\n";
      out += "  }\n";
    }

    out += "\n  static constexpr bool hasExternFields = true;\n";
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

  out += "};\n\n";
}

// --- Struct implementation (source) ---

// Helper to generate the parse loop body. When captureInfo is provided, extern fields
// and context-threaded struct fields use the _capture array.
struct CFieldCaptureInfo {
  int CaptureIdx = -1;         // index into _capture array, or -1
  bool IsDeferred = false;     // true for context-threaded struct field
};

static void generateParseBody(std::string &out, const CStructDef &s, const std::unordered_set<std::string> &enumNames,
                               const CCodegenOptions &opts, const CPerfectHash &ph,
                               const std::unordered_map<uint32_t, size_t> &hashToIndex,
                               const std::unordered_map<size_t, int> &fieldBitMap,
                               int requiredCount,
                               const std::unordered_map<size_t, CFieldCaptureInfo> *captureInfo = nullptr)
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

      // Determine capture behavior for this field
      int captureIdx = -1;
      bool captureIsDeferred = false;
      if (captureInfo) {
        auto cit = captureInfo->find(fi);
        if (cit != captureInfo->end()) {
          captureIdx = cit->second.CaptureIdx;
          captureIsDeferred = cit->second.IsDeferred;
        }
      }

      out += std::format("        case {}:\n", h);
      out += std::format("          if (keyLen == {} && memcmp(key, \"{}\", {}) == 0) {{\n",
                         f.Name.size(), f.Name, f.Name.size());
      generateParseField(out, f, enumNames, foundBit, 6, opts.PascalCaseFields, captureIdx, captureIsDeferred);
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

static void generateStructImpl(std::string &out, const CStructDef &s, const std::unordered_set<std::string> &enumNames, const CCodegenOptions &opts)
{
  std::string sn = opts.StructPrefix + s.Name;
  CExternAnalysis ea = analyzeExternFields(s, opts.PascalCaseFields);
  bool hasExtern = ea.HasExternDirect || ea.HasExternNested;
  int captureCount = (int)ea.ExternFields.size() + (int)ea.NestedContextFields.size();

  // Compute perfect hash (uses original IDL field names for JSON key matching)
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

  // Build capture info map for the capture-aware overload
  std::unordered_map<size_t, CFieldCaptureInfo> captureInfoMap;
  if (hasExtern) {
    // Build field name → field index map
    std::unordered_map<std::string, size_t> fieldNameToIndex;
    for (size_t i = 0; i < s.Fields.size(); i++)
      fieldNameToIndex[fieldCppName(s.Fields[i].Name, opts.PascalCaseFields)] = i;

    int captureIdx = 0;
    for (auto &ef : ea.ExternFields) {
      auto it = fieldNameToIndex.find(ef.FieldName);
      if (it != fieldNameToIndex.end())
        captureInfoMap[it->second] = {captureIdx, false};
      captureIdx++;
    }
    for (auto &nc : ea.NestedContextFields) {
      auto it = fieldNameToIndex.find(nc.FieldName);
      if (it != fieldNameToIndex.end())
        captureInfoMap[it->second] = {captureIdx, true};
      captureIdx++;
    }
  }

  // --- parse() ---
  if (hasExtern) {
    out += std::format("bool {}::parse(const char *buf, size_t bufSize) {{\n", sn);
    out += "  JsonScanner s{buf, buf + bufSize};\n";
    out += "  return parseImpl(s, nullptr);\n";
    out += "}\n\n";
  } else {
    out += std::format("bool {}::parse(const char *buf, size_t bufSize) {{\n", sn);
    out += "  JsonScanner s{buf, buf + bufSize};\n";
    out += "  return parseImpl(s);\n";
    out += "}\n\n";
  }

  // --- parseImpl(s) ---
  if (hasExtern) {
    out += std::format("bool {}::parseImpl(JsonScanner &s) {{\n", sn);
    out += "  return parseImpl(s, nullptr);\n";
    out += "}\n\n";

    // --- parseImpl(s, _capture) ---
    out += std::format("bool {}::parseImpl(JsonScanner &s, std::string *_capture) {{\n", sn);
    generateParseBody(out, s, enumNames, opts, *ph, hashToIndex, fieldBitMap, requiredCount, &captureInfoMap);
    out += "}\n\n";

    // --- parseWithCapture(buf, bufSize, _capture) ---
    out += std::format("bool {}::parseWithCapture(const char *buf, size_t bufSize, std::string (&_capture)[{}]) {{\n", sn, captureCount);
    out += "  JsonScanner s{buf, buf + bufSize};\n";
    out += "  return parseImpl(s, _capture);\n";
    out += "}\n\n";
  } else {
    out += std::format("bool {}::parseImpl(JsonScanner &s) {{\n", sn);
    generateParseBody(out, s, enumNames, opts, *ph, hashToIndex, fieldBitMap, requiredCount);
    out += "}\n\n";
  }

  // --- Verbose parse (when enabled) ---
  if (opts.VerboseMode) {
    // parseVerbose() with ParseError
    out += std::format("bool {}::parseVerbose(const char *buf, size_t bufSize, ParseError &error) {{\n", sn);
    out += "  VerboseJsonScanner s{buf, buf + bufSize, buf, &error};\n";
    out += "  return parseVerboseImpl(s);\n";
    out += "}\n\n";

    // parseVerboseImpl()
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

  // --- serialize() ---
  if (hasExtern) {
    out += std::format("std::string {}::serialize() const {{\n", sn);
    out += "  std::string out;\n";
    out += "  serializeImpl(out, nullptr);\n";
    out += "  return out;\n";
    out += "}\n\n";
  } else {
    out += std::format("std::string {}::serialize() const {{\n", sn);
    out += "  std::string out;\n";
    out += "  serializeImpl(out);\n";
    out += "  return out;\n";
    out += "}\n\n";
  }

  // --- serializeImpl() ---
  if (hasExtern) {
    out += std::format("void {}::serializeImpl(std::string &out) const {{\n", sn);
    out += "  serializeImpl(out, nullptr);\n";
    out += "}\n\n";

    // --- serializeImpl(out, _capture) ---
    out += std::format("void {}::serializeImpl(std::string &out, const std::string *_capture) const {{\n", sn);
    out += "  out += '{';\n";
    bool first = true;
    for (size_t i = 0; i < s.Fields.size(); i++) {
      int captureIdx = -1;
      bool captureIsDeferred = false;
      auto cit = captureInfoMap.find(i);
      if (cit != captureInfoMap.end()) {
        captureIdx = cit->second.CaptureIdx;
        captureIsDeferred = cit->second.IsDeferred;
      }
      generateSerializeField(out, s.Fields[i], enumNames, 1, first, opts.PascalCaseFields, captureIdx, captureIsDeferred);
    }
    out += "  out += '}';\n";
    out += "}\n\n";
  } else {
    out += std::format("void {}::serializeImpl(std::string &out) const {{\n", sn);
    out += "  out += '{';\n";
    bool first = true;
    for (auto &f : s.Fields)
      generateSerializeField(out, f, enumNames, 1, first, opts.PascalCaseFields);
    out += "  out += '}';\n";
    out += "}\n\n";
  }
}

// --- Main entry point ---

CCodegenResult generateCode(const CIdlFile &file, const std::string &headerName, const CCodegenOptions &opts)
{
  CCodegenResult result;
  auto &header = result.Header;
  auto &source = result.Source;

  // Collect enum names (both local and imported for type resolution)
  std::unordered_set<std::string> enumNames;
  for (auto &e : file.Enums)
    enumNames.insert(e.Name);

  // Check if any local struct has tagged schema
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

  // Emit #include for extern type headers (local only, not imported)
  for (auto &ext : file.ExternTypes) {
    if (!ext.IsImported)
      header += std::format("#include \"{}\"\n", ext.IncludePath);
  }
  header += "\n";

  // Emit #include for each include directive
  for (auto &inc : file.Includes) {
    // Transform "foo/bar.idl" → "foo/bar.idl.h"
    header += std::format("#include \"{}.h\"\n", inc.Path);
  }
  if (!file.Includes.empty())
    header += "\n";

  // Forward declarations for scanners
  header += "struct JsonScanner;\n";
  if (opts.VerboseMode) {
    header += parseErrorCode;
    header += "struct VerboseJsonScanner;\n";
  }
  header += "\n";

  // Enum declarations (local only)
  generateEnumDeclarations(header, file, opts.PascalCaseFields);

  // Struct forward declarations (local only)
  for (auto &s : file.Structs) {
    if (s.IsMixin || s.IsImported) continue;
    header += std::format("struct {};\n", opts.StructPrefix + s.Name);
  }
  header += "\n";

  // Struct declarations (local only)
  for (auto &s : file.Structs) {
    if (s.IsMixin || s.IsImported) continue;
    generateStructDecl(header, s, enumNames, opts);
  }

  // === Source ===
  source += "// Generated by idltool — do not edit\n";
  source += std::format("#include \"{}\"\n", headerName);
  source += "#include <cstring>\n";
  source += "#include <cstdlib>\n\n";

  // Scanner definitions
  if (opts.Standalone)
    source += jsonScannerCode;

  if (opts.VerboseMode)
    source += verboseJsonScannerCode;

  // JSON write helpers
  source += jsonHelperCode;
  source += "\n";

  // Enum definitions (local only)
  generateEnumDefinitions(source, file, opts.PascalCaseFields);

  // Struct implementations (local only)
  for (auto &s : file.Structs) {
    if (s.IsMixin || s.IsImported) continue;
    generateStructImpl(source, s, enumNames, opts);
  }

  return result;
}
