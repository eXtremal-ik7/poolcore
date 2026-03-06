#pragma once

#include "ast.h"
#include <string>

struct CCodegenOptions {
  std::string StructPrefix;      // prefix for user struct names (e.g., "C")
  bool Standalone = true;        // include JsonScanner in output
  bool PascalCaseFields = false; // capitalize first letter of field names in C++
};

struct CCodegenResult {
  std::string Header;
  std::string Source;
};

// Generate C++ header + source from resolved AST
CCodegenResult generateCode(const CIdlFile &file, const std::string &headerName, const CCodegenOptions &opts = {});
