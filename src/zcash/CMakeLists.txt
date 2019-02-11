find_path(ZCASH_INCLUDE_DIR "src/zcbenchmarks.h"
  PATH ${ROOT_SOURCE_DIR}/zcash
)

file(GLOB ZCASH_DEPENDS_FILES "${ROOT_SOURCE_DIR}/zcash/depends/*")
find_path(ZCASH_DEPENDS_INCLUDE_DIR
  NAMES
    "db_cxx.h"
  HINTS
    ${ZCASH_DEPENDS_FILES}
  PATH_SUFFIXES
    include
  NO_DEFAULT_PATH
)

message("ZEC directory is ${ZCASH_INCLUDE_DIR}")
message("ZEC depends directory is ${ZCASH_DEPENDS_INCLUDE_DIR}")

add_library(poolrpczcash STATIC
  ${CMAKE_SOURCE_DIR}/poolrpc/poolrpc.cpp
  zcash.cpp
)

target_include_directories(poolrpczcash PUBLIC
  ${ZCASH_INCLUDE_DIR}/src
  ${ZCASH_INCLUDE_DIR}/src/config
  ${ZCASH_INCLUDE_DIR}/src/univalue/include
  ${ZCASH_DEPENDS_INCLUDE_DIR}
)

target_compile_definitions(poolrpczcash PUBLIC -DHAVE_CONFIG_H)
