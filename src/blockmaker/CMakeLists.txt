# Backend library
add_library(blockmaker STATIC
  scrypt.cpp
  scrypt-sse2.cpp

  btc.cpp
  btcLike.cpp
  dgb.cpp
  doge.cpp
  ltc.cpp
  xpm.cpp
  zec.cpp
)

target_link_libraries(blockmaker ${MPIR_LIBRARIES})

add_dependencies(blockmaker
  mpir
)
