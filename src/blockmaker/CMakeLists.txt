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
  aes.c
  sha256.c
  sha3.c
  scrypt.c
  x11.cpp

  ethash.c

  blake.c
  bmw.c
  cubehash.cpp
  echo.cpp
  groestl.c
  jh.c
  luffa.cpp
  simd.cpp
  shavite.cpp
  skein.cpp
  odocrypt.cpp

  btc.cpp
  btcLike.cpp
  dash.cpp
  dgb.cpp
  doge.cpp
  eth.cpp
  ltc.cpp
  xpm.cpp
  zec.cpp

  stratumWork.cpp

  protocol.proto
)

target_link_libraries(blockmaker
  OpenSSL::SSL
  OpenSSL::Crypto
  libsodium::libsodium
  protobuf::libprotobuf
  ${BIGNUM_LIBRARIES}
)

protobuf_generate(TARGET blockmaker)

if (MSVC)
  set_source_files_properties("protocol.pb.cc" PROPERTIES COMPILE_FLAGS "/wd4244 /wd4267")
endif()

target_include_directories(blockmaker PUBLIC ${RAPIDJSON_INCLUDE_DIRECTORY})
