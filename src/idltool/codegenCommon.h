#pragma once

#include "ast.h"
#include <string>
#include <vector>
#include <optional>
#include <unordered_set>

// --- Helpers ---

std::string indent(int level);
std::string cppScalarType(EScalarType t);
std::string cppFieldType(const CFieldDef &f, const std::unordered_set<std::string> &enumNames, const std::string &structPrefix = "");
std::string cppDefault(const CFieldDef &f, const std::unordered_set<std::string> &enumNames, bool pascalCase = false);
std::string fieldCppName(const std::string &name, bool pascalCase);

bool isEnum(const std::string &refName, const std::unordered_set<std::string> &enumNames);
bool isStructRef(const CFieldDef &f, const std::unordered_set<std::string> &enumNames);
bool isExternField(const CFieldDef &f);

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

// --- Serialize generation ---

void emitSerializeValue(std::string &code, const CFieldDef &f,
                        const std::string &valueName,
                        const std::unordered_set<std::string> &enumNames,
                        int ind);
void emitSerializeArrayElem(std::string &code, const CFieldDef &f,
                            const std::string &valueName,
                            const std::unordered_set<std::string> &enumNames,
                            int ind);
// captureIdx: for extern fields, index into _capture array; -1 = no capture (write "")
// captureIsDeferred: true for context-threaded struct fields (use pre-serialized _capture[idx])
void generateSerializeField(std::string &code, const CFieldDef &f, const std::unordered_set<std::string> &enumNames, int ind, bool &first, bool pascalCase = false, int captureIdx = -1, bool captureIsDeferred = false);

// --- Scanner code strings ---

extern const char *jsonScannerCode;
extern const char *jsonHelperCode;
