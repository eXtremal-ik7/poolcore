protobuf_generate_cpp(ProtoSources ProtoHeaders protocol.proto)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_library(poolinstances STATIC
  fabric.cpp
  ${ProtoSources}
)

target_link_libraries(poolinstances asyncioextras-0.5 blockmaker ${TBB_IMPORTED_TARGETS} ${MPIR_LIBRARIES} protobuf::libprotobuf)
add_dependencies(poolinstances mpir)
