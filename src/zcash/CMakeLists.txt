find_path(ZCASH_INCLUDE_DIR "src/zcbenchmarks.h"
  PATH ${ROOT_SOURCE_DIR}/zcash
)

add_library(poolrpczcash STATIC
  ${CMAKE_SOURCE_DIR}/poolrpc/poolrpc.cpp
  zcash.cpp
)

message(${ZCASH_INCLUDE_DIR})
target_include_directories(poolrpczcash PUBLIC
  ${ZCASH_INCLUDE_DIR}/src
  ${ZCASH_INCLUDE_DIR}/src/config
  ${ZCASH_INCLUDE_DIR}/depends/x86_64-unknown-linux-gnu/include/
)

target_compile_definitions(poolrpczcash PUBLIC -DHAVE_CONFIG_H)
