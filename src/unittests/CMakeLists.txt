add_executable(unittests
  main.cpp
  statsDataTests.cpp
  taggedTests.cpp
  idltoolTests.cpp
)

# Generate test.idl.h from test.idl using idltool
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test.idl.h
  COMMAND idltool ${CMAKE_CURRENT_SOURCE_DIR}/test.idl -o ${CMAKE_CURRENT_BINARY_DIR}/test.idl.h --diag
  DEPENDS idltool ${CMAKE_CURRENT_SOURCE_DIR}/test.idl
  COMMENT "Generating test.idl.h"
)
add_custom_target(generate_test_idl DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/test.idl.h)
add_dependencies(unittests generate_test_idl)

target_include_directories(unittests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(unittests
  GTest::gtest
  ${LIBRARIES}
)
