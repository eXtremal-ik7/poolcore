find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

BISON_TARGET(IdlParser parser.y ${CMAKE_CURRENT_BINARY_DIR}/parser.cpp
  DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser.h)
FLEX_TARGET(IdlLexer lexer.l ${CMAKE_CURRENT_BINARY_DIR}/lexer.cpp
  COMPILE_FLAGS "--header-file=${CMAKE_CURRENT_BINARY_DIR}/lexer.h")
ADD_FLEX_BISON_DEPENDENCY(IdlLexer IdlParser)

add_executable(idltool
  main.cpp ast.cpp codegen.cpp codegenCommon.cpp codegenVerbose.cpp
  ${FLEX_IdlLexer_OUTPUTS} ${BISON_IdlParser_OUTPUTS})
target_include_directories(idltool PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
