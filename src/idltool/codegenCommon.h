#pragma once

#include "ast.h"
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_set>

// --- Helpers ---

std::string indent(int level);
std::string cppScalarType(EScalarType t);
std::string cppFieldType(const CFieldDef &f, const std::unordered_set<std::string> &enumNames, const std::string &structPrefix = "");
std::string cppVariantAltType(const CVariantAlt &alt, const std::unordered_set<std::string> &enumNames, const std::string &structPrefix = "");
std::string cppDefault(const CFieldDef &f, const std::unordered_set<std::string> &enumNames, bool pascalCase = false);
std::string fieldCppName(const std::string &name, bool pascalCase);
CFieldDef variantAltAsField(const CVariantAlt &alt, const std::string &name = "");

bool isEnum(const std::string &refName, const std::unordered_set<std::string> &enumNames);
bool isStructRef(const CFieldDef &f, const std::unordered_set<std::string> &enumNames);
bool isMappedField(const CFieldDef &f);

struct CSerializeCodeBuilder {
  explicit CSerializeCodeBuilder(std::string &code);

  void writeLiteral(int ind, std::string_view literal);
  void appendRaw(std::string_view text);
  void flush();

private:
  std::string &Code_;
  int PendingIndent_ = -1;
  std::string PendingLiteral_;
};

// --- Perfect hash ---

struct CPerfectHash {
  uint32_t Seed;
  uint32_t Mult;
  uint32_t Mod;
};

uint32_t computeHash(const char *key, size_t len, uint32_t seed, uint32_t mult, uint32_t mod);
std::optional<CPerfectHash> findPerfectHash(const std::vector<std::string> &keys);

// --- Enum generation ---

void generateEnumDeclarations(std::string &out, const CIdlFile &file, bool pascalCase = false);
void generateEnumDefinitions(std::string &out, const CIdlFile &file, bool pascalCase = false);

// --- Wire type info ---

struct WireTypeInfo {
  const char *readMethod;  // JsonScanner method
  const char *writeFunc;   // jsonWrite* function
  const char *cppType;     // C++ wire type
};

WireTypeInfo getWireTypeInfo(const std::string &wireType);

void emitMappedInlineValueSerialize(CSerializeCodeBuilder &code,
                                    const std::string &mappedTypeName,
                                    const std::string &mappedWireType,
                                    const std::string &valueName,
                                    int ind);
void emitMappedInlineNestedArraySerialize(CSerializeCodeBuilder &code,
                                          const std::string &mappedTypeName,
                                          const std::string &mappedWireType,
                                          const std::vector<CArrayDim> &dims, int dimIndex,
                                          const std::string &valueName,
                                          int ind);

// --- Serialize generation ---

void emitSerializeValue(CSerializeCodeBuilder &code, const CFieldDef &f,
                        const std::string &valueName,
                        const std::unordered_set<std::string> &enumNames,
                        int ind);
void emitSerializeArrayElem(CSerializeCodeBuilder &code, const CFieldDef &f,
                            const std::string &valueName,
                            const std::unordered_set<std::string> &enumNames,
                            int ind);
void emitSerializeExpr(CSerializeCodeBuilder &code, const CFieldDef &f,
                       const std::string &valueName,
                       const std::unordered_set<std::string> &enumNames,
                       int ind);
void generateSerializeField(CSerializeCodeBuilder &code, const CFieldDef &f, const std::unordered_set<std::string> &enumNames, int ind, bool &first, bool pascalCase = false, bool useRuntimeComma = false, std::string_view commaVar = "");

// --- Variant/FixedArray serialize helpers ---

void emitVariantSerialize(CSerializeCodeBuilder &code, const CFieldDef &f,
                           const std::string &valueName,
                           const std::unordered_set<std::string> &enumNames,
                           int ind);

void emitNestedArraySerialize(CSerializeCodeBuilder &code, const CFieldDef &f,
                               const std::vector<CArrayDim> &dims, int dimIndex,
                               const std::string &valueName,
                               const std::unordered_set<std::string> &enumNames,
                               int ind);

// --- JSON helper usage tracking ---

struct CJsonHelperUsage {
  bool WriteString = false;
  bool WriteInt = false;
  bool WriteUInt = false;
  bool WriteDouble = false;
  bool WriteBool = false;

  bool any() const
  {
    return WriteString || WriteInt || WriteUInt || WriteDouble || WriteBool;
  }
};

struct CJsonReadUsage {
  bool ReadString = false;
  bool ReadBool = false;
  bool ReadInt = false;
  bool ReadUInt = false;
  bool ReadDouble = false;

  bool any() const
  {
    return ReadString || ReadBool || ReadInt || ReadUInt || ReadDouble;
  }
};
