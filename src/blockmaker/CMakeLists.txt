# Backend library
add_library(blockmaker STATIC
  scrypt.cpp
  scrypt-sse2.cpp

  btc.cpp
  ltc.cpp
  xpm.cpp
)

target_link_libraries(blockmaker ${MPIR_LIBRARIES})

add_dependencies(blockmaker
  mpir
)
