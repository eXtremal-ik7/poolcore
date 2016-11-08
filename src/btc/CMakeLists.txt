add_library(poolrpcbtc STATIC
  ${CMAKE_SOURCE_DIR}/poolrpc/poolrpc.cpp
  btc.cpp
)

target_include_directories(poolrpcbtc PUBLIC
  /data/build/bitcoin/src
  /data/build/bitcoin/build/src
  /data/build/bitcoin/build/src/config
)

target_compile_definitions(poolrpcbtc PUBLIC -DHAVE_CONFIG_H)
