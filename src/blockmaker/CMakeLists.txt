# Backend library
add_library(blockmaker STATIC
  btc.cpp
  xpm.cpp
)

target_link_libraries(blockmaker ${MPIR_LIBRARIES})

add_dependencies(blockmaker
  mpir
)
