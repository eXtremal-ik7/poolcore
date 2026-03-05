#pragma once

#include "ast.h"
#include <string>

struct CCodegenOptions {
  bool Standalone = true; // include JsonScanner in output
  bool DiagMode = false;  // include DiagJsonScanner + parseDiagImpl
};

// Generate C++ header from resolved AST
std::string generateCode(const CIdlFile &file, const CCodegenOptions &opts = {});
