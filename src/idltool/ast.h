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
  Double
};

// How a field is declared
enum class EFieldKind {
  Required,       // field: Type
  Optional,       // field: Type = default
  OptionalObject, // field: Type?
  Array,          // field: [Type]
  OptionalArray   // field: [Type]?
};

// Shape of field declaration in IDL
enum class ETypeShape {
  Plain,          // field: Type
  OptionalObject, // field: Type?
  Array,          // field: [Type]
  OptionalArray   // field: [Type]?
};

// Type of a field — scalar or reference to struct/enum
struct CFieldType {
  bool IsScalar = false;
  EScalarType Scalar = EScalarType::String;
  std::string RefName; // non-empty if referencing struct/enum
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
  int Line = 0;
};

struct CMixinRef {
  std::string Name;
  int Line = 0;
};

struct CStructDef {
  std::string Name;
  bool IsMixin = false;
  std::vector<std::variant<CMixinRef, CFieldDef>> Members;
  // After mixin resolution — flat list of fields
  std::vector<CFieldDef> Fields;
  int Line = 0;
};

struct CEnumDef {
  std::string Name;
  std::string BaseType; // "string"
  std::vector<std::string> Values;
  int Line = 0;
};

struct CIdlFile {
  std::vector<CStructDef> Structs; // includes mixins
  std::vector<CEnumDef> Enums;
};

// Parse IDL from file, returns false on error
bool parseIdlFile(const char *filename, CIdlFile &out);

// Post-process AST: resolve mixins, validate types, topological sort
bool resolveAst(CIdlFile &file);

// Dump AST (for --dump-ast)
void dumpAst(const CIdlFile &file);

// Check if a scalar type name is valid, returns optional EScalarType
std::optional<EScalarType> parseScalarType(const std::string &name);

const char *scalarTypeName(EScalarType t);
