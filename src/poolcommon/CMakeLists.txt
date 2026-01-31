add_library(poolcommon STATIC
  bech32.cpp
  coroutineJoin.cpp
  datFile.cpp
  file.cpp
  taskHandler.cpp
  totp.cpp
  utils.cpp
)

target_link_libraries(poolcommon
  OpenSSL::SSL
  OpenSSL::Crypto
  ${BIGNUM_LIBRARIES}
)
