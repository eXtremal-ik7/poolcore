#pragma once

#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

// Scalar IDL types
enum class EScalarType {
  String,
  Bool,
  Int32,
  Uint32,
  Int64,
  Uint64,
  Double,
  Seconds,
  Minutes,
  Hours
};

// How a field is declared
enum class EFieldKind {
  Required,        // field: Type
  Optional,        // field: Type = default
  OptionalObject,  // field: Type?          — omitted when nullopt
  NullableObject,  // field: Type??         — null when nullopt
  Array,           // field: [Type]
  OptionalArray,   // field: [Type]?        — omitted when nullopt
  NullableArray    // field: [Type]??       — null when nullopt
};

// Shape of field declaration in IDL
enum class ETypeShape {
  Plain,           // field: Type
  OptionalObject,  // field: Type?
  NullableObject,  // field: Type??
  Array,           // field: [Type]
  OptionalArray,   // field: [Type]?
  NullableArray    // field: [Type]??
};

// Type of a field — scalar or reference to struct/enum/mapped
struct CFieldType {
  bool IsScalar = false;
  bool IsMapped = false;
  EScalarType Scalar = EScalarType::String;
  std::string RefName; // non-empty if referencing struct/enum/mapped type
  std::string MappedCppType; // C++ type from mapped type definition
  ETypeShape Shape = ETypeShape::Plain;
};

// Default value kinds
enum class EDefaultKind {
  None,
  String,
  Int,
  Float,
  Bool,
  RuntimeNow,       // @now
  RuntimeNowOffset  // @(now - N)
};

struct CDefaultValue {
  EDefaultKind Kind = EDefaultKind::None;
  std::string StringVal;
  int64_t IntVal = 0;
  double FloatVal = 0.0;
  bool BoolVal = false;
};

// AST nodes
struct CFieldDef {
  std::string Name;
  CFieldType Type;
  EFieldKind Kind = EFieldKind::Required;
  CDefaultValue Default;
  int Tag = 0;  // 0 = no tag, 1..63 = tagged field for schema()
  int Line = 0;
};

struct CMixinRef {
  std::string Name;
  int Line = 0;
};

struct CContextDecl {
  std::string MappedTypeName;
  int Line = 0;
};

struct CCppBlock {
  std::string Code;
  int Line = 0;
};

struct CStructDef {
  std::string Name;
  bool IsMixin = false;
  bool IsImported = false;
  bool HasTaggedSchema = false;
  std::vector<std::variant<CMixinRef, CFieldDef, CContextDecl, CCppBlock>> Members;
  // After mixin resolution — flat list of fields
  std::vector<CFieldDef> Fields;
  // Context declarations from 'context money;' members
  std::vector<std::string> ContextDecls;
  // Raw C++ code blocks to emit inside the struct
  std::vector<std::string> CppBlocks;
  int Line = 0;
};

struct CEnumDef {
  std::string Name;
  std::string BaseType; // "string"
  std::vector<std::string> Values;
  bool IsImported = false;
  int Line = 0;
};

struct CIncludeDirective {
  std::string Path;
  int Line = 0;
};

struct CMappedTypeDef {
  std::string Name;
  std::string CppType;
  std::string JsonWireType;
  std::string IncludePath;
  std::optional<EScalarType> ContextType;
  int Line = 0;
  bool IsImported = false;
};

struct CIdlFile {
  std::vector<CIncludeDirective> Includes;
  std::vector<CStructDef> Structs; // includes mixins
  std::vector<CEnumDef> Enums;
  std::vector<CMappedTypeDef> MappedTypes;
};

// Parse IDL from file, returns false on error
bool parseIdlFile(const char *filename, CIdlFile &out);

// Process include directives: parse included files, merge imported types
bool processIncludes(CIdlFile &file, const std::vector<std::string> &includePaths);

// Post-process AST: resolve mixins, validate types, topological sort
bool resolveAst(CIdlFile &file);

// Dump AST (for --dump-ast)
void dumpAst(const CIdlFile &file);

// Check if a scalar type name is valid, returns optional EScalarType
std::optional<EScalarType> parseScalarType(const std::string &name);

const char *scalarTypeName(EScalarType t);
