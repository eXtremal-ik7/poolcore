#include "codegen.h"
#include "codegenCommon.h"
#include "codegenDiag.h"
#include <cstdio>
#include <cstdint>
#include <format>
#include <unordered_set>
#include <unordered_map>

// --- Minimal parse generation ---

static void generateParseScalar(std::string &out, const CFieldDef &f,
                                 const std::unordered_set<std::string> &enumNames,
                                 int foundBit, int ind)
{
  std::string in = indent(ind);

  if (isEnum(f.Type.RefName, enumNames)) {
    out += std::format("{}if (!([&]() {{ const char *eStr; size_t eLen; "
                       "return s.readString(eStr, eLen) && parseE{}(eStr, eLen, {}); }}())) valid = false;\n",
                       in, f.Type.RefName, f.Name);
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
  }

  out += std::format("{}if (!s.{}({})) valid = false;\n", in, readMethod, f.Name);
  if (foundBit >= 0)
    out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
}

static void generateParseField(std::string &out, const CFieldDef &f,
                                const std::unordered_set<std::string> &enumNames,
                                int foundBit, int ind)
{
  std::string in = indent(ind);

  switch (f.Kind) {
    case EFieldKind::Required:
    case EFieldKind::Optional: {
      if (isStructRef(f, enumNames)) {
        out += std::format("{}if (!{}.parseImpl(s)) valid = false;\n", in, f.Name);
        if (foundBit >= 0)
          out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
      } else {
        generateParseScalar(out, f, enumNames, foundBit, ind);
      }
      break;
    }

    case EFieldKind::OptionalObject: {
      out += std::format("{}if (s.readNull()) {{ /* ok, remains nullopt */ }}\n", in);
      if (isStructRef(f, enumNames)) {
        out += std::format("{}else if (!{}.emplace().parseImpl(s)) valid = false;\n", in, f.Name);
      } else {
        out += std::format("{}else {{\n", in);
        out += std::format("{}  {}.emplace();\n", in, f.Name);
        CFieldDef tmp = f;
        tmp.Name = std::format("(*{})", f.Name);
        tmp.Kind = EFieldKind::Required;
        generateParseScalar(out, tmp, enumNames, -1, ind + 1);
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
        out += std::format("{}    if (!{}.emplace_back().parseImpl(s)) {{ valid = false; break; }}\n", in, f.Name);
      } else if (isEnum(f.Type.RefName, enumNames)) {
        out += std::format("{}    {{\n", in);
        out += std::format("{}      const char *eStr; size_t eLen;\n", in);
        out += std::format("{}      {}.emplace_back();\n", in, f.Name);
        out += std::format("{}      if (!s.readString(eStr, eLen) || !parseE{}(eStr, eLen, {}.back())) "
                           "{{ valid = false; break; }}\n", in, f.Type.RefName, f.Name);
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
          out += std::format("{}    {{ std::string tmp; if (!s.{}(tmp)) {{ valid = false; break; }} "
                             "{}.push_back(std::move(tmp)); }}\n", in, readMethod, f.Name);
        } else {
          out += std::format("{}    {{ {} tmp; if (!s.{}(tmp)) {{ valid = false; break; }} "
                             "{}.push_back(tmp); }}\n", in, cppType, readMethod, f.Name);
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
      out += std::format("{}  {}.emplace();\n", in, f.Name);
      CFieldDef tmp = f;
      tmp.Name = std::format("(*{})", f.Name);
      tmp.Kind = EFieldKind::Array;
      generateParseField(out, tmp, enumNames, -1, ind + 1);
      out += std::format("{}}}\n", in);
      break;
    }
  }
}

// --- Struct generation ---

static void generateStruct(std::string &out, const CStructDef &s,
                            const std::unordered_set<std::string> &enumNames,
                            bool diagMode)
{
  // Collect field names for perfect hash
  std::vector<std::string> fieldNames;
  for (auto &f : s.Fields)
    fieldNames.push_back(f.Name);

  auto ph = findPerfectHash(fieldNames);
  if (!ph) {
    fprintf(stderr, "warning: cannot find perfect hash for struct '%s', using mod=%zu\n",
            s.Name.c_str(), fieldNames.size() * 4);
    ph = CPerfectHash{0, 31, (uint32_t)(fieldNames.size() * 4)};
  }

  // Count required fields (Required scalars/structs + Array are required)
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

  // --- Struct declaration ---
  out += std::format("struct {} {{\n", s.Name);

  // Fields
  for (auto &f : s.Fields) {
    std::string type = cppFieldType(f, enumNames);
    std::string def = cppDefault(f, enumNames);
    if (!def.empty())
      out += std::format("  {} {} = {};\n", type, f.Name, def);
    else
      out += std::format("  {} {};\n", type, f.Name);
  }
  out += "\n";

  // parse()
  out += "  bool parse(const char *buf, size_t bufSize) {\n";
  out += "    JsonScanner s{buf, buf + bufSize};\n";
  out += "    return parseImpl(s);\n";
  out += "  }\n\n";

  // parseImpl()
  out += "  bool parseImpl(JsonScanner &s) {\n";
  out += "    if (!s.expectChar('{')) return false;\n";
  out += "    bool valid = true;\n";
  if (requiredCount > 0)
    out += "    uint64_t found = 0;\n";
  out += "    s.skipWhitespace();\n";
  out += "    if (s.p < s.end && *s.p != '}') {\n";
  out += "      for (;;) {\n";
  out += "        const char *key;\n";
  out += "        size_t keyLen;\n";

  if (s.Fields.empty()) {
    out += "        if (!s.readString(key, keyLen)) return false;\n";
    out += "        if (!s.expectChar(':')) return false;\n";
    out += "        s.skipValue();\n";
  } else {
    out += "        uint32_t keyHash;\n";
    out += std::format("        if (!s.readStringHash(key, keyLen, keyHash, {}, {})) return false;\n",
                       ph->Seed, ph->Mult);
    out += "        if (!s.expectChar(':')) return false;\n";
    out += std::format("        switch (keyHash % {}) {{\n", ph->Mod);

    for (uint32_t h = 0; h < ph->Mod; h++) {
      auto it = hashToIndex.find(h);
      if (it == hashToIndex.end()) continue;

      size_t fi = it->second;
      auto &f = s.Fields[fi];
      int foundBit = -1;
      auto bitIt = fieldBitMap.find(fi);
      if (bitIt != fieldBitMap.end())
        foundBit = bitIt->second;

      out += std::format("          case {}:\n", h);
      out += std::format("            if (keyLen == {} && memcmp(key, \"{}\", {}) == 0) {{\n",
                         f.Name.size(), f.Name, f.Name.size());
      generateParseField(out, f, enumNames, foundBit, 7);
      out += "            } else { s.skipValue(); }\n";
      out += "            break;\n";
    }

    out += "          default:\n";
    out += "            s.skipValue();\n";
    out += "            break;\n";
    out += "        }\n";
  }

  out += "        s.skipWhitespace();\n";
  out += "        if (s.p < s.end && *s.p == ',') { s.p++; continue; }\n";
  out += "        break;\n";
  out += "      }\n";
  out += "    }\n";
  out += "    if (!s.expectChar('}')) return false;\n";

  if (requiredCount > 0) {
    uint64_t mask = ((uint64_t)1 << requiredCount) - 1;
    out += std::format("    return valid && (found & 0x{:x}) == 0x{:x};\n", mask, mask);
  } else {
    out += "    return valid;\n";
  }
  out += "  }\n\n";

  // Diagnostic parse (when enabled)
  if (diagMode) {
    // parse() with ParseError
    out += "  bool parse(const char *buf, size_t bufSize, ParseError &error) {\n";
    out += "    DiagJsonScanner s{buf, buf + bufSize, buf, &error};\n";
    out += "    return parseDiagImpl(s);\n";
    out += "  }\n\n";

    // parseDiagImpl()
    out += "  bool parseDiagImpl(DiagJsonScanner &s) {\n";
    out += "    if (!s.expectChar('{')) return false;\n";
    if (requiredCount > 0)
      out += "    uint64_t found = 0;\n";
    out += "    s.skipWhitespace();\n";
    out += "    if (s.p < s.end && *s.p != '}') {\n";
    out += "      for (;;) {\n";
    out += "        const char *key;\n";
    out += "        size_t keyLen;\n";

    if (s.Fields.empty()) {
      out += "        if (!s.readString(key, keyLen)) return false;\n";
      out += "        if (!s.expectChar(':')) return false;\n";
      out += "        s.skipValue();\n";
    } else {
      out += "        uint32_t keyHash;\n";
      out += std::format("        if (!s.readStringHash(key, keyLen, keyHash, {}, {})) return false;\n",
                         ph->Seed, ph->Mult);
      out += "        if (!s.expectChar(':')) return false;\n";
      out += std::format("        switch (keyHash % {}) {{\n", ph->Mod);

      for (uint32_t h = 0; h < ph->Mod; h++) {
        auto it = hashToIndex.find(h);
        if (it == hashToIndex.end()) continue;

        size_t fi = it->second;
        auto &f = s.Fields[fi];
        int foundBit = -1;
        auto bitIt = fieldBitMap.find(fi);
        if (bitIt != fieldBitMap.end())
          foundBit = bitIt->second;

        out += std::format("          case {}:\n", h);
        out += std::format("            if (keyLen == {} && memcmp(key, \"{}\", {}) == 0) {{\n",
                           f.Name.size(), f.Name, f.Name.size());

        // Diagnostic parse: same structure but with error context
        std::string diagFieldCode;
        generateDiagParseField(diagFieldCode, f, enumNames, foundBit, 7);
        out += diagFieldCode;

        out += "            } else { s.skipValue(); }\n";
        out += "            break;\n";
      }

      out += "          default:\n";
      out += "            s.skipValue();\n";
      out += "            break;\n";
      out += "        }\n";
    }

    out += "        s.skipWhitespace();\n";
    out += "        if (s.p < s.end && *s.p == ',') { s.p++; continue; }\n";
    out += "        break;\n";
    out += "      }\n";
    out += "    }\n";
    out += "    if (!s.expectChar('}')) return false;\n";

    if (requiredCount > 0) {
      uint64_t mask = ((uint64_t)1 << requiredCount) - 1;
      out += std::format("    if ((found & 0x{:x}) != 0x{:x}) {{\n", mask, mask);
      // Report first missing required field
      for (size_t i = 0; i < s.Fields.size(); i++) {
        auto bitIt2 = fieldBitMap.find(i);
        if (bitIt2 != fieldBitMap.end()) {
          out += std::format("      if (!(found & ((uint64_t)1 << {}))) "
                             "{{ s.setError(\"missing required field '{}'\"); return false; }}\n",
                             bitIt2->second, s.Fields[i].Name);
        }
      }
      out += "    }\n";
    }

    out += "    return true;\n";
    out += "  }\n\n";
  }

  // serialize()
  out += "  std::string serialize() const {\n";
  out += "    std::string out;\n";
  out += "    serializeImpl(out);\n";
  out += "    return out;\n";
  out += "  }\n\n";

  // serializeImpl()
  out += "  void serializeImpl(std::string &out) const {\n";
  out += "    out += '{';\n";
  bool first = true;
  for (auto &f : s.Fields)
    generateSerializeField(out, f, enumNames, 2, first);
  out += "    out += '}';\n";
  out += "  }\n";

  out += "};\n\n";
}

// --- Main entry point ---

std::string generateCode(const CIdlFile &file, const CCodegenOptions &opts)
{
  std::string out;

  out += "// Generated by idltool — do not edit\n";
  out += "#pragma once\n\n";
  out += "#include <string>\n";
  out += "#include <vector>\n";
  out += "#include <optional>\n";
  out += "#include <cstdint>\n";
  out += "#include <cstring>\n";
  out += "#include <cstdlib>\n";
  out += "#include <ctime>\n\n";

  if (opts.Standalone)
    out += jsonScannerCode;

  if (opts.DiagMode)
    out += diagJsonScannerCode;

  out += "\n";

  // Collect enum names
  std::unordered_set<std::string> enumNames;
  for (auto &e : file.Enums)
    enumNames.insert(e.Name);

  // Generate enums
  generateEnumParsers(out, file);

  // Forward declarations
  for (auto &s : file.Structs) {
    if (s.IsMixin) continue;
    out += std::format("struct {};\n", s.Name);
  }
  out += "\n";

  // Generate structs (topologically sorted)
  for (auto &s : file.Structs) {
    if (s.IsMixin) continue;
    generateStruct(out, s, enumNames, opts.DiagMode);
  }

  return out;
}
