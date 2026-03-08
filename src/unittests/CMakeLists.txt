add_executable(unittests
  main.cpp
  statsDataTests.cpp
  taggedTests.cpp
  idltoolTests.cpp
  jsonScannerTests.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/test.idl.cpp
)

# Generate test.idl.h and test.idl.cpp from test.idl using idltool
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test.idl.h ${CMAKE_CURRENT_BINARY_DIR}/test.idl.cpp
  COMMAND idltool ${CMAKE_CURRENT_SOURCE_DIR}/test.idl -o ${CMAKE_CURRENT_BINARY_DIR}/test.idl.h
  DEPENDS idltool ${CMAKE_CURRENT_SOURCE_DIR}/test.idl
  COMMENT "Generating test.idl.h and test.idl.cpp"
)
add_custom_target(generate_test_idl DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/test.idl.h ${CMAKE_CURRENT_BINARY_DIR}/test.idl.cpp)
add_dependencies(unittests generate_test_idl)

target_include_directories(unittests PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(unittests PRIVATE
  IDLTOOL_BINARY_PATH=\"$<TARGET_FILE:idltool>\"
)

target_link_libraries(unittests
  GTest::gtest
  ${LIBRARIES}
)
