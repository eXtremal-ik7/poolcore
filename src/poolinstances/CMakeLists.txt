protobuf_generate_cpp(ProtoSources ProtoHeaders protocol.proto)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_library(poolinstances STATIC
  fabric.cpp
  stratumMsg.cpp
  ${ProtoSources}
)

target_link_libraries(poolinstances ${TBB_IMPORTED_TARGETS})
add_dependencies(poolinstances mpir)
