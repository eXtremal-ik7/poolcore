protobuf_generate_cpp(ProtoSources ProtoHeaders protocol.proto)

add_library(poolinstances STATIC
  fabric.cpp
  ${ProtoSources}
)

target_link_libraries(poolinstances blockmaker ${TBB_IMPORTED_TARGETS} ${MPIR_LIBRARIES} protobuf::libprotobuf)
add_dependencies(poolinstances mpir)
