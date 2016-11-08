# XPM pool rpc

find_path(PRIMECOIN_INCLUDE_DIR "prime.h"
  PATH ${ROOT_SOURCE_DIR}/primecoin/src
)

add_library(poolrpcxpm STATIC
  ${CMAKE_SOURCE_DIR}/poolrpc/poolrpc.cpp
  xpm.cpp
)

target_include_directories(poolrpcxpm PUBLIC
  ${PRIMECOIN_INCLUDE_DIR}
)

# crypto-currency wallet emulators for poolrpc
add_executable(xpmstub xpm_stub.cpp)

target_link_libraries(xpmstub
  poolrpcxpm
  ${AIO_LIBRARY}
  ${P2P_LIBRARY}
  ${OPENSSL_SSL_LIBRARY}
  ${OPENSSL_CRYPTO_LIBRARY}
  pthread #TODO: move to boost::thread
  ${Boost_SYSTEM_LIBRARY}
  ${Boost_THREAD_LIBRARY}
)
