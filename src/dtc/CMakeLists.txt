# DTC pool rpc

find_path(DATACOIN_INCLUDE_DIR "prime.h"
  PATH ${ROOT_SOURCE_DIR}/datacoin/src
)

add_library(poolrpcdtc STATIC
  ${CMAKE_SOURCE_DIR}/poolrpc/poolrpc.cpp
  dtc.cpp
)

target_include_directories(poolrpcdtc PUBLIC
  ${DATACOIN_INCLUDE_DIR}
)
