# Backend library
add_library(blockmaker STATIC
  ethash.c
  scrypt.cpp
  scrypt-sse2.cpp
  tiny_sha3.c

  btc.cpp
  btcLike.cpp
  dgb.cpp
  doge.cpp
  eth.cpp
  ltc.cpp
  xpm.cpp
  zec.cpp
)

target_link_libraries(blockmaker
  OpenSSL::SSL
  OpenSSL::Crypto
  libsodium::libsodium
  ${BIGNUM_LIBRARIES}
  poolinstances
)

target_include_directories(blockmaker PUBLIC ${RAPIDJSON_INCLUDE_DIRECTORY})
