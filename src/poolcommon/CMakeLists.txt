add_library(poolcommon STATIC
  arith_uint256.cpp
  bech32.cpp
  bigNum.cpp
  coroutineJoin.cpp
  file.cpp
  taskHandler.cpp
  uint256.cpp
  utils.cpp
)

target_link_libraries(poolcommon ${MPIR_LIBRARIES})

add_dependencies(poolcommon
  mpir
)
