protobuf_generate_cpp(ProtoSources ProtoHeaders protocol.proto)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_library(poolinstances STATIC
  fabric.cpp
  stratumMsg.cpp
  ${ProtoSources}
)

if (MSVC)
  set_source_files_properties(${ProtoSources} PROPERTIES COMPILE_FLAGS "/wd4244 /wd4267")
endif()

target_link_libraries(poolinstances ${TBB_IMPORTED_TARGETS})
add_dependencies(poolinstances mpir)
