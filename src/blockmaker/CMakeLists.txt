# Backend library
add_library(blockmaker STATIC
  btc.cpp
  xpm.cpp
)

add_dependencies(blockmaker
  mpir
)
