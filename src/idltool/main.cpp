#include "ast.h"
#include "codegen.h"
#include <cstdio>
#include <cstring>
#include <string>

static void usage()
{
  fprintf(stderr,
    "Usage: idltool [options] <input.idl>\n"
    "  -o <file>      output file (default: <input>.idl.h)\n"
    "  --stdout       write to stdout\n"
    "  --dump-ast     dump AST and exit\n"
    "  --diag         generate diagnostic parser with detailed errors\n"
  );
}

int main(int argc, char **argv)
{
  const char *inputFile = nullptr;
  const char *outputFile = nullptr;
  bool toStdout = false;
  bool dumpAstMode = false;
  bool diagMode = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      outputFile = argv[++i];
    } else if (strcmp(argv[i], "--stdout") == 0) {
      toStdout = true;
    } else if (strcmp(argv[i], "--dump-ast") == 0) {
      dumpAstMode = true;
    } else if (strcmp(argv[i], "--diag") == 0) {
      diagMode = true;
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

  // Generate
  CCodegenOptions opts;
  opts.DiagMode = diagMode;
  std::string code = generateCode(file, opts);

  // Output
  if (toStdout) {
    fwrite(code.data(), 1, code.size(), stdout);
  } else {
    std::string outPath;
    if (outputFile) {
      outPath = outputFile;
    } else {
      outPath = std::string(inputFile) + ".h";
    }
    FILE *f = fopen(outPath.c_str(), "w");
    if (!f) {
      fprintf(stderr, "cannot open '%s' for writing: %s\n", outPath.c_str(), strerror(errno));
      return 1;
    }
    fwrite(code.data(), 1, code.size(), f);
    fclose(f);
    fprintf(stderr, "generated %s\n", outPath.c_str());
  }

  return 0;
}
