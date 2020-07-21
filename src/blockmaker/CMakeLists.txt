# Backend library
add_library(blockmaker STATIC
  btc.cpp
  xpm.cpp
)

target_link_libraries(blockmaker poolcommon)

add_dependencies(blockmaker
  mpir
)
