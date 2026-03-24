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
  Hours,
  HexData
};

enum class ENullInPolicy {
  Deny,
  Allow,
  Required
};

enum class EEmptyOutPolicy {
  Omit,
  Null
};

// How a field is declared
enum class EFieldKind {
  Required,            // field: Type
  HasDefault,          // field: Type = default   — uses default value when field is absent in JSON
  OptionalObject,      // field: optional<Type>
  Array,               // field: [Type]
  OptionalArray,       // field: optional<[Type]>
  FixedArray,          // field: [Type; N]
  OptionalFixedArray,  // field: optional<[Type; N]>
  Variant,             // field: variant(T1, T2, T3)
  OptionalVariant,     // field: optional<variant(T1, T2, T3)>
  Map,                 // field: map<Type>         — JSON object with dynamic string keys
  OptionalMap          // field: optional<map<Type>>
};

// Shape of field declaration in IDL
enum class ETypeShape {
  Plain,                // field: Type
  OptionalObject,       // field: optional<Type>
  Array,                // field: [Type]
  OptionalArray,        // field: optional<[Type]>
  FixedArray,           // field: [Type; N]
  OptionalFixedArray,   // field: optional<[Type; N]>
  Variant,              // field: variant(T1, T2, T3)
  OptionalVariant,      // field: optional<variant(T1, T2, T3)>
  Map,                  // field: map<Type>
  OptionalMap           // field: optional<map<Type>>
};

// What kind of element an array holds
enum class EArrayElementKind {
  Plain,     // [T]                      — direct scalar/struct/enum/mapped
  Optional,  // [optional<T>(...)]       — nullable element (std::optional<T>)
  Variant,   // [variant(T1, T2, ...)]   — variant element (std::variant<...>)
  Map,       // [map<T>]                 — map element (std::unordered_map<std::string, T>)
};

// Inner array dimension for multi-dimensional arrays
struct CArrayDim {
  int FixedSize = 0; // 0 = dynamic (std::vector), >0 = fixed (std::array)
};

// A single alternative in a variant type
struct CVariantAlt {
  bool IsScalar = false;
  EScalarType Scalar = EScalarType::String;
  std::string RefName;
  bool IsUserType = false;
  bool IsDerived = false;
  std::string MappedCppType;
  std::string MappedWireType;
  std::vector<CArrayDim> Dims; // outermost first for array alternatives
  // Kept for compatibility with existing AST dump/logic; backtracking parse does not require it.
  std::string DiscriminatingField;
};

// Type of a field — scalar or reference to struct/enum/usertype/derived
struct CFieldType {
  bool IsScalar = false;
  bool IsUserType = false;
  bool IsDerived = false;
  EScalarType Scalar = EScalarType::String;
  std::string RefName; // non-empty if referencing struct/enum/mapped type
  std::string MappedCppType; // C++ type from mapped type definition
  std::string MappedWireType; // JSON wire type from mapped type definition
  ETypeShape Shape = ETypeShape::Plain;
  ENullInPolicy NullIn = ENullInPolicy::Deny;
  EEmptyOutPolicy EmptyOut = EEmptyOutPolicy::Omit;

  // Fixed-size array: 0 = dynamic, >0 = std::array<T, N>
  int FixedSize = 0;

  // Inner dimensions for multi-dimensional arrays (outermost first)
  std::vector<CArrayDim> InnerDims;

  // Variant alternatives (non-empty → variant type or variant array element)
  std::vector<CVariantAlt> Alternatives;

  // Array element kind — applies to leaf elements of arrays
  EArrayElementKind ArrayElementKind = EArrayElementKind::Plain;
  ENullInPolicy ElementNullIn = ENullInPolicy::Deny;      // for Optional array elements
  EEmptyOutPolicy ElementEmptyOut = EEmptyOutPolicy::Omit; // for Optional array elements
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

struct COptionalPolicySpec {
  ENullInPolicy NullIn = ENullInPolicy::Deny;
  EEmptyOutPolicy EmptyOut = EEmptyOutPolicy::Omit;
  bool HasNullIn = false;
  bool HasEmptyOut = false;
};

// AST nodes
struct CFieldDef {
  std::string Name;
  CFieldType Type;
  EFieldKind Kind = EFieldKind::Required;
  ENullInPolicy NullIn = ENullInPolicy::Deny;
  EEmptyOutPolicy EmptyOut = EEmptyOutPolicy::Omit;
  CDefaultValue Default;
  int Tag = 0;  // 0 = no tag, 1..63 = tagged field for schema()
  int Line = 0;
};

struct CMixinRef {
  std::string Name;
  int Line = 0;
};

struct CCtxGroupDecl {
  std::vector<std::string> FieldNames;
  int Line = 0;
};

struct CGenerateDecl {
  bool Parse = false;
  bool ParseVerbose = false;
  bool Serialize = false;
  bool SerializeFlat = false;
  int Line = 0;
};

struct CCppBlock {
  std::string Code;
  int Line = 0;
};

struct CExtensionsDecl {
  bool Comments = false;
  int Line = 0;
};

struct CFlagsDecl {
  bool SkipUnknown = false;
  bool ArrayLayout = false;
  bool TopLevel = false;
  int Line = 0;
};

using CStructMember = std::variant<CMixinRef, CFieldDef, CCtxGroupDecl, CCppBlock, CGenerateDecl, CExtensionsDecl, CFlagsDecl>;

struct CStructDef {
  std::string Name;
  bool IsMixin = false;
  bool IsImported = false;
  bool HasTaggedSchema = false;
  std::vector<CStructMember> Members;
  // After mixin resolution — flat list of fields
  std::vector<CFieldDef> Fields;
  // Explicit context groups from '.ctxgroup(field1, field2, ...)' members
  std::vector<CCtxGroupDecl> ContextGroups;
  // Raw C++ code blocks to emit inside the struct
  std::vector<std::string> CppBlocks;
  // Generate flags from '.generate(...)' directive
  CGenerateDecl GenerateFlags;
  bool HasGenerateDecl = false;
  // Extensions from '.extensions(...)' directive
  bool CommentsEnabled = false;
  // Flags from '.flags(...)' directive
  bool SkipUnknownFields = false;
  bool ArrayLayout = false;
  bool TopLevel = false;
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

struct CUserTypeDef {
  std::string Name;
  std::string CppType;
  std::string IncludePath;
  int Line = 0;
  bool IsImported = false;
};

struct CDerivedTypeDef {
  std::string Name;
  std::string CppType;          // C++ target type (string literal from IDL)
  std::string WireTypeName;     // scalar (wire type for JSON read/write)
  std::string ContextTypeName;  // scalar (context parameter type)
  std::string IncludePath;
  // Resolved during validation:
  EScalarType ContextType = EScalarType::Uint32;
  int Line = 0;
  bool IsImported = false;
};

struct CIdlFile {
  std::vector<CIncludeDirective> Includes;
  std::vector<CStructDef> Structs; // includes mixins
  std::vector<CEnumDef> Enums;
  std::vector<CUserTypeDef> UserTypes;
  std::vector<CDerivedTypeDef> DerivedTypes;
};

// Parse IDL from file, returns false on error
bool parseIdlFile(const char *filename, CIdlFile &out);

// Process include directives: parse included files, merge imported types
// rootFile is the path of the main IDL file (used to prevent self-inclusion cycles)
bool processIncludes(CIdlFile &file, const std::vector<std::string> &includePaths, const char *rootFile);

// Post-process AST: resolve mixins, validate types, topological sort
bool resolveAst(CIdlFile &file);

// Dump AST (for --dump-ast)
void dumpAst(const CIdlFile &file);

// Check if a scalar type name is valid, returns optional EScalarType
std::optional<EScalarType> parseScalarType(const std::string &name);

const char *idlScalarTypeName(EScalarType t);

inline bool isOptionalKind(EFieldKind kind)
{
  switch (kind) {
    case EFieldKind::OptionalObject:
    case EFieldKind::OptionalArray:
    case EFieldKind::OptionalFixedArray:
    case EFieldKind::OptionalVariant:
    case EFieldKind::OptionalMap:
      return true;
    default:
      return false;
  }
}

inline bool isOptionalField(const CFieldDef &f)
{
  return isOptionalKind(f.Kind);
}

inline bool fieldAllowsNullInput(const CFieldDef &f)
{
  return isOptionalField(f) && f.NullIn != ENullInPolicy::Deny;
}

inline bool fieldRequiresPresence(const CFieldDef &f)
{
  return isOptionalField(f) && f.NullIn == ENullInPolicy::Required;
}

inline bool fieldEmptyOutIsNull(const CFieldDef &f)
{
  return isOptionalField(f) && f.EmptyOut == EEmptyOutPolicy::Null;
}
