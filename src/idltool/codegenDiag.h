#pragma once

#include "ast.h"
#include <string>
#include <unordered_set>

// DiagJsonScanner inline code for generated headers
extern const char *diagJsonScannerCode;

// Generate diagnostic parse code for a single field
void generateDiagParseField(std::string &out, const CFieldDef &f,
                            const std::unordered_set<std::string> &enumNames,
                            int foundBit, int ind);
