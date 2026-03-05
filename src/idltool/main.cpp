#include "ast.h"
#include "codegen.h"
#include <cstdio>
#include <cstring>
#include <string>

static void usage()
{
  fprintf(stderr,
    "Usage: idltool [options] <input.idl>\n"
    "  -o <file>           output header file (default: <input>.idl.h)\n"
    "  --dump-ast          dump AST and exit\n"
    "  --parser-verbose    generate verbose parser with detailed errors\n"
    "  --struct-prefix P   prefix for generated struct names (e.g., C)\n"
    "  --pascal-case       capitalize first letter of field names in C++\n"
  );
}

int main(int argc, char **argv)
{
  const char *inputFile = nullptr;
  const char *outputFile = nullptr;
  bool dumpAstMode = false;
  bool verboseMode = false;
  const char *structPrefix = "";
  bool pascalCase = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      outputFile = argv[++i];
    } else if (strcmp(argv[i], "--dump-ast") == 0) {
      dumpAstMode = true;
    } else if (strcmp(argv[i], "--parser-verbose") == 0) {
      verboseMode = true;
    } else if (strcmp(argv[i], "--struct-prefix") == 0 && i + 1 < argc) {
      structPrefix = argv[++i];
    } else if (strcmp(argv[i], "--pascal-case") == 0) {
      pascalCase = true;
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "unknown option: %s\n", argv[i]);
      usage();
      return 1;
    } else {
      inputFile = argv[i];
    }
  }

  if (!inputFile) {
    usage();
    return 1;
  }

  // Parse
  CIdlFile file;
  if (!parseIdlFile(inputFile, file))
    return 1;

  // Resolve
  if (!resolveAst(file))
    return 1;

  if (dumpAstMode) {
    dumpAst(file);
    return 0;
  }

  // Determine output paths
  std::string headerPath;
  if (outputFile) {
    headerPath = outputFile;
  } else {
    headerPath = std::string(inputFile) + ".h";
  }

  std::string sourcePath;
  if (headerPath.size() >= 2 && headerPath.substr(headerPath.size() - 2) == ".h") {
    sourcePath = headerPath.substr(0, headerPath.size() - 2) + ".cpp";
  } else {
    sourcePath = headerPath + ".cpp";
  }

  // Extract header basename for #include
  std::string headerName = headerPath;
  auto slashPos = headerName.find_last_of('/');
  if (slashPos != std::string::npos)
    headerName = headerName.substr(slashPos + 1);

  // Generate
  CCodegenOptions opts;
  opts.StructPrefix = structPrefix;
  opts.VerboseMode = verboseMode;
  opts.PascalCaseFields = pascalCase;
  CCodegenResult result = generateCode(file, headerName, opts);

  // Write header
  FILE *f = fopen(headerPath.c_str(), "w");
  if (!f) {
    fprintf(stderr, "cannot open '%s' for writing: %s\n", headerPath.c_str(), strerror(errno));
    return 1;
  }
  fwrite(result.Header.data(), 1, result.Header.size(), f);
  fclose(f);
  fprintf(stderr, "generated %s\n", headerPath.c_str());

  // Write source
  f = fopen(sourcePath.c_str(), "w");
  if (!f) {
    fprintf(stderr, "cannot open '%s' for writing: %s\n", sourcePath.c_str(), strerror(errno));
    return 1;
  }
  fwrite(result.Source.data(), 1, result.Source.size(), f);
  fclose(f);
  fprintf(stderr, "generated %s\n", sourcePath.c_str());

  return 0;
}
