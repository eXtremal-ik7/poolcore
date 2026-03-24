add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/types.idl.h ${CMAKE_CURRENT_BINARY_DIR}/types.idl.cpp
  COMMAND idltool ${CMAKE_CURRENT_SOURCE_DIR}/types.idl -o ${CMAKE_CURRENT_BINARY_DIR}/types.idl.h --struct-prefix C --pascal-case
  DEPENDS idltool ${CMAKE_CURRENT_SOURCE_DIR}/types.idl
  COMMENT "Generating types.idl.h and types.idl.cpp"
)
add_custom_target(generate_types_idl DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/types.idl.h ${CMAKE_CURRENT_BINARY_DIR}/types.idl.cpp)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/hash.idl.h ${CMAKE_CURRENT_BINARY_DIR}/hash.idl.cpp
  COMMAND idltool ${CMAKE_CURRENT_SOURCE_DIR}/hash.idl -o ${CMAKE_CURRENT_BINARY_DIR}/hash.idl.h --struct-prefix C --pascal-case
  DEPENDS idltool ${CMAKE_CURRENT_SOURCE_DIR}/hash.idl
  COMMENT "Generating hash.idl.h and hash.idl.cpp"
)
add_custom_target(generate_hash_idl DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/hash.idl.h ${CMAKE_CURRENT_BINARY_DIR}/hash.idl.cpp)

add_library(poolcommon STATIC
  bech32.cpp
  datFile.cpp
  file.cpp
  httpClient.cpp
  taskHandler.cpp
  totp.cpp
  utils.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/types.idl.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/hash.idl.cpp
)
add_dependencies(poolcommon generate_types_idl generate_hash_idl)

target_link_libraries(poolcommon
  OpenSSL::SSL
  OpenSSL::Crypto
  ${BIGNUM_LIBRARIES}
)
