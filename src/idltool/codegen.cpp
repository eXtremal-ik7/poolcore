#include "codegen.h"
#include "codegenCommon.h"
#include "codegenVerbose.h"
#include <cstdio>
#include <cstdint>
#include <format>
#include <unordered_set>
#include <unordered_map>

// --- Minimal parse generation ---

static void generateParseScalar(std::string &out, const CFieldDef &f, const std::unordered_set<std::string> &enumNames, int foundBit, int ind, bool pascalCase)
{
  std::string in = indent(ind);
  std::string cn = fieldCppName(f.Name, pascalCase);

  if (isEnum(f.Type.RefName, enumNames)) {
    out += std::format("{}if (!([&]() {{ const char *eStr; size_t eLen; "
                       "return s.readString(eStr, eLen) && parseE{}(eStr, eLen, {}); }}())) valid = false;\n",
                       in, f.Type.RefName, cn);
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

  out += std::format("{}if (!s.{}({})) valid = false;\n", in, readMethod, cn);
  if (foundBit >= 0)
    out += std::format("{}else found |= (uint64_t)1 << {};\n", in, foundBit);
}

static void generateParseField(std::string &out, const CFieldDef &f, const std::unordered_set<std::string> &enumNames, int foundBit, int ind, bool pascalCase)
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
        generateParseScalar(out, f, enumNames, foundBit, ind, pascalCase);
      }
      break;
    }

    case EFieldKind::OptionalObject: {
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

static void generateStructDecl(std::string &out, const CStructDef &s, const std::unordered_set<std::string> &enumNames, const CCodegenOptions &opts)
{
  std::string sn = opts.StructPrefix + s.Name;
  out += std::format("struct {} {{\n", sn);

  // Fields
  for (auto &f : s.Fields) {
    std::string type = cppFieldType(f, enumNames, opts.StructPrefix);
    std::string cn = fieldCppName(f.Name, opts.PascalCaseFields);
    std::string def = cppDefault(f, enumNames);
    if (!def.empty())
      out += std::format("  {} {} = {};\n", type, cn, def);
    else
      out += std::format("  {} {};\n", type, cn);
  }
  out += "\n";

  // Method declarations
  out += "  bool parse(const char *buf, size_t bufSize);\n";
  out += "  bool parseImpl(JsonScanner &s);\n";
  if (opts.VerboseMode) {
    out += "  bool parseVerbose(const char *buf, size_t bufSize, ParseError &error);\n";
    out += "  bool parseVerboseImpl(VerboseJsonScanner &s);\n";
  }
  out += "  std::string serialize() const;\n";
  out += "  void serializeImpl(std::string &out) const;\n";

  out += "};\n\n";
}

// --- Struct implementation (source) ---

static void generateStructImpl(std::string &out, const CStructDef &s, const std::unordered_set<std::string> &enumNames, const CCodegenOptions &opts)
{
  std::string sn = opts.StructPrefix + s.Name;

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

  // --- parse() ---
  out += std::format("bool {}::parse(const char *buf, size_t bufSize) {{\n", sn);
  out += "  JsonScanner s{buf, buf + bufSize};\n";
  out += "  return parseImpl(s);\n";
  out += "}\n\n";

  // --- parseImpl() ---
  out += std::format("bool {}::parseImpl(JsonScanner &s) {{\n", sn);
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
      generateParseField(out, f, enumNames, foundBit, 6, opts.PascalCaseFields);
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
  out += "}\n\n";

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
  out += std::format("std::string {}::serialize() const {{\n", sn);
  out += "  std::string out;\n";
  out += "  serializeImpl(out);\n";
  out += "  return out;\n";
  out += "}\n\n";

  // --- serializeImpl() ---
  out += std::format("void {}::serializeImpl(std::string &out) const {{\n", sn);
  out += "  out += '{';\n";
  bool first = true;
  for (auto &f : s.Fields)
    generateSerializeField(out, f, enumNames, 1, first, opts.PascalCaseFields);
  out += "  out += '}';\n";
  out += "}\n\n";
}

// --- Main entry point ---

CCodegenResult generateCode(const CIdlFile &file, const std::string &headerName, const CCodegenOptions &opts)
{
  CCodegenResult result;
  auto &header = result.Header;
  auto &source = result.Source;

  // Collect enum names
  std::unordered_set<std::string> enumNames;
  for (auto &e : file.Enums)
    enumNames.insert(e.Name);

  // === Header ===
  header += "// Generated by idltool — do not edit\n";
  header += "#pragma once\n\n";
  header += "#include <string>\n";
  header += "#include <vector>\n";
  header += "#include <optional>\n";
  header += "#include <cstdint>\n";
  header += "#include <ctime>\n\n";

  // Forward declarations for scanners
  header += "struct JsonScanner;\n";
  if (opts.VerboseMode) {
    header += parseErrorCode;
    header += "struct VerboseJsonScanner;\n";
  }
  header += "\n";

  // Enum declarations
  generateEnumDeclarations(header, file);

  // Struct forward declarations
  for (auto &s : file.Structs) {
    if (s.IsMixin) continue;
    header += std::format("struct {};\n", opts.StructPrefix + s.Name);
  }
  header += "\n";

  // Struct declarations (fields + method declarations only)
  for (auto &s : file.Structs) {
    if (s.IsMixin) continue;
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

  // Enum definitions
  generateEnumDefinitions(source, file);

  // Struct implementations
  for (auto &s : file.Structs) {
    if (s.IsMixin) continue;
    generateStructImpl(source, s, enumNames, opts);
  }

  return result;
}
