#include "ast.h"
#include "parser.h"
#include "lexer.h"
#include <climits>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>

std::optional<EScalarType> parseScalarType(const std::string &name)
{
  if (name == "string") return EScalarType::String;
  if (name == "bool")   return EScalarType::Bool;
  if (name == "int32")  return EScalarType::Int32;
  if (name == "uint32") return EScalarType::Uint32;
  if (name == "int64")  return EScalarType::Int64;
  if (name == "uint64") return EScalarType::Uint64;
  if (name == "double") return EScalarType::Double;
  if (name == "chrono::seconds") return EScalarType::Seconds;
  if (name == "chrono::minutes") return EScalarType::Minutes;
  if (name == "chrono::hours")   return EScalarType::Hours;
  return std::nullopt;
}

const char *scalarTypeName(EScalarType t)
{
  switch (t) {
    case EScalarType::String:  return "string";
    case EScalarType::Bool:    return "bool";
    case EScalarType::Int32:   return "int32";
    case EScalarType::Uint32:  return "uint32";
    case EScalarType::Int64:   return "int64";
    case EScalarType::Uint64:  return "uint64";
    case EScalarType::Double:  return "double";
    case EScalarType::Seconds: return "chrono::seconds";
    case EScalarType::Minutes: return "chrono::minutes";
    case EScalarType::Hours:   return "chrono::hours";
  }
  return "unknown";
}

bool parseIdlFile(const char *filename, CIdlFile &out)
{
  FILE *f = fopen(filename, "r");
  if (!f) {
    fprintf(stderr, "cannot open '%s': %s\n", filename, strerror(errno));
    return false;
  }

  yyscan_t scanner;
  yylex_init(&scanner);
  yyset_in(f, scanner);

  int result = yyparse(&out, scanner);

  yylex_destroy(scanner);
  fclose(f);
  return result == 0;
}

// --- Include processing ---

static bool processIncludesImpl(CIdlFile &file, const std::vector<std::string> &includePaths,
                                 std::unordered_set<std::string> &visited)
{
  for (auto &inc : file.Includes) {
    // Try each include path
    std::filesystem::path resolved;
    bool found = false;
    for (auto &dir : includePaths) {
      auto candidate = std::filesystem::path(dir) / inc.Path;
      if (std::filesystem::exists(candidate)) {
        resolved = std::filesystem::canonical(candidate);
        found = true;
        break;
      }
    }
    if (!found) {
      fprintf(stderr, "line %d: cannot find included file '%s'\n", inc.Line, inc.Path.c_str());
      return false;
    }

    std::string resolvedStr = resolved.string();
    if (!visited.insert(resolvedStr).second)
      continue; // already included — skip

    // Parse included file
    CIdlFile included;
    if (!parseIdlFile(resolvedStr.c_str(), included))
      return false;

    // Build include paths for the included file: its directory + original paths
    std::vector<std::string> childPaths;
    childPaths.push_back(resolved.parent_path().string());
    for (auto &p : includePaths)
      childPaths.push_back(p);

    // Recursively process includes
    if (!processIncludesImpl(included, childPaths, visited))
      return false;

    // Merge imported types
    for (auto &e : included.Enums) {
      e.IsImported = true;
      file.Enums.push_back(std::move(e));
    }
    for (auto &s : included.Structs) {
      s.IsImported = true;
      file.Structs.push_back(std::move(s));
    }
    for (auto &ext : included.ExternTypes) {
      ext.IsImported = true;
      file.ExternTypes.push_back(std::move(ext));
    }
  }
  return true;
}

bool processIncludes(CIdlFile &file, const std::vector<std::string> &includePaths)
{
  std::unordered_set<std::string> visited;
  return processIncludesImpl(file, includePaths, visited);
}

// --- Resolve AST ---

static bool resolveMixins(CIdlFile &file)
{
  // Build index: name → struct pointer
  std::unordered_map<std::string, CStructDef *> structIndex;
  for (auto &s : file.Structs)
    structIndex[s.Name] = &s;

  // Resolve each struct
  for (auto &s : file.Structs) {
    s.Fields.clear();
    std::unordered_set<std::string> fieldNames;
    std::unordered_set<std::string> mixinVisited;

    for (auto &member : s.Members) {
      if (auto *ref = std::get_if<CMixinRef>(&member)) {
        auto it = structIndex.find(ref->Name);
        if (it == structIndex.end()) {
          fprintf(stderr, "line %d: unknown mixin '%s'\n", ref->Line, ref->Name.c_str());
          return false;
        }
        if (!it->second->IsMixin) {
          fprintf(stderr, "line %d: '%s' is not a mixin\n", ref->Line, ref->Name.c_str());
          return false;
        }
        if (!mixinVisited.insert(ref->Name).second) {
          fprintf(stderr, "line %d: duplicate mixin '%s'\n", ref->Line, ref->Name.c_str());
          return false;
        }
        // Copy mixin fields
        // Mixin must have been resolved already if it only contains fields (no nested mixins)
        // For simplicity, mixins cannot include other mixins
        for (auto &mm : it->second->Members) {
          if (std::holds_alternative<CMixinRef>(mm)) {
            fprintf(stderr, "line %d: mixin '%s' contains nested mixin references (not supported)\n",
                    ref->Line, ref->Name.c_str());
            return false;
          }
          auto &field = std::get<CFieldDef>(mm);
          if (!fieldNames.insert(field.Name).second) {
            fprintf(stderr, "line %d: duplicate field '%s' (from mixin '%s')\n",
                    ref->Line, field.Name.c_str(), ref->Name.c_str());
            return false;
          }
          s.Fields.push_back(field);
        }
      } else {
        auto &field = std::get<CFieldDef>(member);
        if (!fieldNames.insert(field.Name).second) {
          fprintf(stderr, "line %d: duplicate field '%s'\n", field.Line, field.Name.c_str());
          return false;
        }
        s.Fields.push_back(field);
      }
    }
  }
  return true;
}

static bool validateTypes(CIdlFile &file)
{
  // Build indices (include both local and imported)
  std::unordered_set<std::string> structNames;
  std::unordered_set<std::string> enumNames;
  std::unordered_set<std::string> externNames;
  std::unordered_map<std::string, CEnumDef *> enumIndex;

  for (auto &s : file.Structs)
    if (!s.IsMixin)
      structNames.insert(s.Name);
  for (auto &e : file.Enums) {
    enumNames.insert(e.Name);
    enumIndex[e.Name] = &e;
  }
  for (auto &ext : file.ExternTypes)
    externNames.insert(ext.Name);

  for (auto &s : file.Structs) {
    if (s.IsMixin || s.IsImported) continue;

    // Build field name set for context validation
    std::unordered_set<std::string> fieldNameSet;
    for (auto &f : s.Fields)
      fieldNameSet.insert(f.Name);

    for (auto &f : s.Fields) {
      if (!f.Type.IsScalar && !f.Type.RefName.empty()) {
        // Check extern types first
        if (externNames.count(f.Type.RefName)) {
          f.Type.IsExtern = true;
        } else {
          bool found = structNames.count(f.Type.RefName) || enumNames.count(f.Type.RefName);
          if (!found) {
            fprintf(stderr, "line %d: unknown type '%s' for field '%s'\n",
                    f.Line, f.Type.RefName.c_str(), f.Name.c_str());
            return false;
          }
          // Mark if it's an enum
          if (enumNames.count(f.Type.RefName)) {
            f.Type.IsScalar = true; // enums are parsed as strings
          }
        }
      }

      // Validate context(X) annotation
      if (!f.ContextField.empty()) {
        if (!fieldNameSet.count(f.ContextField)) {
          fprintf(stderr, "line %d: context field '%s' not found in struct '%s'\n",
                  f.Line, f.ContextField.c_str(), s.Name.c_str());
          return false;
        }
      }

      // Validate defaults
      if (f.Default.Kind == EDefaultKind::String && f.Type.IsScalar) {
        // String default for enum → validate value
        if (!f.Type.RefName.empty()) {
          auto it = enumIndex.find(f.Type.RefName);
          if (it != enumIndex.end()) {
            auto &vals = it->second->Values;
            if (std::find(vals.begin(), vals.end(), f.Default.StringVal) == vals.end()) {
              fprintf(stderr, "line %d: invalid default '%s' for enum '%s'\n",
                      f.Line, f.Default.StringVal.c_str(), f.Type.RefName.c_str());
              return false;
            }
          }
        }
      }
    }

    // Validate tags
    bool anyTag = false, anyNoTag = false;
    for (auto &f : s.Fields) {
      if (f.Tag != 0) anyTag = true;
      else anyNoTag = true;
    }
    if (anyTag && anyNoTag) {
      fprintf(stderr, "struct '%s': mixing tagged and untagged fields is not allowed\n", s.Name.c_str());
      return false;
    }
    if (anyTag) {
      std::unordered_set<int> usedTags;
      for (auto &f : s.Fields) {
        if (f.Tag < 1 || f.Tag > 63) {
          fprintf(stderr, "line %d: tag @%d out of range 1..63\n", f.Line, f.Tag);
          return false;
        }
        if (!usedTags.insert(f.Tag).second) {
          fprintf(stderr, "line %d: duplicate tag @%d in struct '%s'\n", f.Line, f.Tag, s.Name.c_str());
          return false;
        }
      }
      s.HasTaggedSchema = true;
    }
  }
  return true;
}

static bool topologicalSort(CIdlFile &file)
{
  // Build adjacency: struct name → set of struct dependencies
  std::unordered_map<std::string, std::unordered_set<std::string>> deps;
  std::unordered_map<std::string, CStructDef *> structIndex;
  std::unordered_set<std::string> enumNames;
  std::unordered_set<std::string> externNames;
  std::unordered_set<std::string> importedNames;

  for (auto &e : file.Enums)
    enumNames.insert(e.Name);
  for (auto &ext : file.ExternTypes)
    externNames.insert(ext.Name);

  // First pass: collect imported names
  for (auto &s : file.Structs) {
    if (!s.IsMixin && s.IsImported)
      importedNames.insert(s.Name);
  }

  // Second pass: build dependency graph for local structs
  std::vector<CStructDef *> nonMixins;
  for (auto &s : file.Structs) {
    if (s.IsMixin || s.IsImported) continue;
    structIndex[s.Name] = &s;
    nonMixins.push_back(&s);
    deps[s.Name]; // ensure entry exists
    for (auto &f : s.Fields) {
      if (!f.Type.RefName.empty() && !enumNames.count(f.Type.RefName) &&
          !importedNames.count(f.Type.RefName) && !externNames.count(f.Type.RefName)) {
        deps[s.Name].insert(f.Type.RefName);
      }
    }
  }

  // Kahn's algorithm
  std::unordered_map<std::string, int> inDeg;
  for (auto &[name, _] : deps) inDeg[name] = 0;
  for (auto &[name, d] : deps)
    for (auto &dep : d)
      inDeg[dep]++; // wrong direction — need reverse

  // Actually: dep → name means name depends on dep, so dep must come first
  // Build reverse: for each dep edge (name → dep), dep should come before name
  // inDegree of name = number of deps it has
  inDeg.clear();
  for (auto &[name, _] : deps) inDeg[name] = 0;
  for (auto &[name, d] : deps)
    inDeg[name] = (int)d.size();

  // Reverse adjacency: dep → list of names that depend on it
  std::unordered_map<std::string, std::vector<std::string>> revAdj;
  for (auto &[name, d] : deps)
    for (auto &dep : d)
      revAdj[dep].push_back(name);

  std::vector<std::string> order;
  std::vector<std::string> queue;
  for (auto &[name, deg] : inDeg)
    if (deg == 0)
      queue.push_back(name);

  while (!queue.empty()) {
    auto cur = queue.back();
    queue.pop_back();
    order.push_back(cur);
    for (auto &dependent : revAdj[cur]) {
      if (--inDeg[dependent] == 0)
        queue.push_back(dependent);
    }
  }

  if (order.size() != nonMixins.size()) {
    fprintf(stderr, "error: circular dependency between structs\n");
    return false;
  }

  // Reorder Structs: imported first, then mixins, then sorted non-mixins
  std::vector<CStructDef> sorted;
  for (auto &s : file.Structs)
    if (s.IsImported)
      sorted.push_back(std::move(s));
  for (auto &s : file.Structs)
    if (s.IsMixin)
      sorted.push_back(std::move(s));
  for (auto &name : order)
    sorted.push_back(std::move(*structIndex[name]));

  file.Structs = std::move(sorted);
  return true;
}

bool resolveAst(CIdlFile &file)
{
  return resolveMixins(file) && validateTypes(file) && topologicalSort(file);
}

void dumpAst(const CIdlFile &file)
{
  for (auto &ext : file.ExternTypes) {
    printf("extern type %s : %s include \"%s\";\n", ext.Name.c_str(), ext.JsonWireType.c_str(), ext.IncludePath.c_str());
  }
  for (auto &e : file.Enums) {
    printf("enum %s : %s { ", e.Name.c_str(), e.BaseType.c_str());
    for (size_t i = 0; i < e.Values.size(); i++) {
      if (i) printf(", ");
      printf("%s", e.Values[i].c_str());
    }
    printf(" }\n");
  }
  for (auto &s : file.Structs) {
    printf("%s %s {\n", s.IsMixin ? "mixin" : "struct", s.Name.c_str());
    for (auto &f : s.Fields) {
      printf("  %s: ", f.Name.c_str());
      if (f.Type.IsScalar && f.Type.RefName.empty())
        printf("%s", scalarTypeName(f.Type.Scalar));
      else if (!f.Type.RefName.empty())
        printf("%s", f.Type.RefName.c_str());
      switch (f.Kind) {
        case EFieldKind::Required: break;
        case EFieldKind::Optional: break;
        case EFieldKind::OptionalObject: printf("?"); break;
        case EFieldKind::Array: printf("[]"); break;
        case EFieldKind::OptionalArray: printf("[]?"); break;
      }
      if (f.Default.Kind != EDefaultKind::None) {
        printf(" = ");
        switch (f.Default.Kind) {
          case EDefaultKind::String: printf("\"%s\"", f.Default.StringVal.c_str()); break;
          case EDefaultKind::Int: printf("%lld", (long long)f.Default.IntVal); break;
          case EDefaultKind::Float: printf("%g", f.Default.FloatVal); break;
          case EDefaultKind::Bool: printf("%s", f.Default.BoolVal ? "true" : "false"); break;
          case EDefaultKind::RuntimeNow: printf("@now"); break;
          case EDefaultKind::RuntimeNowOffset: printf("@(now - %lld)", (long long)f.Default.IntVal); break;
          default: break;
        }
      }
      if (f.Tag > 0)
        printf(" @%d", f.Tag);
      if (!f.ContextField.empty())
        printf(" context(%s)", f.ContextField.c_str());
      printf(";\n");
    }
    printf("}\n");
  }
}
