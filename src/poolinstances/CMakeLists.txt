# Backend library
add_library(poolinstances STATIC
  fabric.cpp

  btc.cpp
  xpm.cpp
)

target_link_libraries(poolinstances ${TBB_IMPORTED_TARGETS} ${MPIR_LIBRARIES})
add_dependencies(poolinstances mpir)
