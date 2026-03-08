#pragma once

#include "ast.h"
#include <string>
#include <unordered_set>

// Generate verbose parse code for a single field
void generateVerboseParseField(std::string &out, const CFieldDef &f, const std::unordered_set<std::string> &enumNames, int foundBit, int ind, bool pascalCase = false);
