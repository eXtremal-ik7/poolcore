add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/poolconfig.idl.h ${CMAKE_CURRENT_BINARY_DIR}/poolconfig.idl.cpp
  COMMAND idltool ${CMAKE_CURRENT_SOURCE_DIR}/poolconfig.idl -o ${CMAKE_CURRENT_BINARY_DIR}/poolconfig.idl.h --struct-prefix C --pascal-case
  DEPENDS idltool ${CMAKE_CURRENT_SOURCE_DIR}/poolconfig.idl
  COMMENT "Generating poolconfig.idl.h and poolconfig.idl.cpp"
)
add_custom_target(generate_poolconfig_idl DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/poolconfig.idl.h ${CMAKE_CURRENT_BINARY_DIR}/poolconfig.idl.cpp)

add_library(poolconfig STATIC ${CMAKE_CURRENT_BINARY_DIR}/poolconfig.idl.cpp)
add_dependencies(poolconfig generate_poolconfig_idl)
