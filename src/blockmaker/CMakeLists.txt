option(LIBPOW_SSE2_ENABLED off)
option(LIBPOW_SHANI_ENABLED off)

if (LIBPOW_SHANI_ENABLED)
  add_definitions(-DLIBPOW_SHANI_ENABLED)
  set_source_files_properties(sha256.c PROPERTIES COMPILE_FLAGS -march=znver1)
endif()

if (LIBPOW_SSE2_ENABLED)
  add_definitions(-DLIBPOW_SSE2_ENABLED)
endif()

# Backend library
add_library(blockmaker STATIC
  sha256.c
  scrypt.c

  ethash.c
  #scrypt.cpp
  #scrypt-sse2.cpp
  tiny_sha3.c

  aes_helper.cpp
  cubehash.cpp
  echo.cpp
  luffa.cpp
  simd.cpp
  shavite.cpp
  skein.cpp

  KeccakP-800-reference.cpp
  odocrypt.cpp

  btc.cpp
  btcLike.cpp
  dgb.cpp
  doge.cpp
  eth.cpp
  ltc.cpp
  xpm.cpp
  zec.cpp

  stratumWork.cpp
)

target_link_libraries(blockmaker
  OpenSSL::SSL
  OpenSSL::Crypto
  libsodium::libsodium
  ${BIGNUM_LIBRARIES}
  poolinstances
)

target_include_directories(blockmaker PUBLIC ${RAPIDJSON_INCLUDE_DIRECTORY})
