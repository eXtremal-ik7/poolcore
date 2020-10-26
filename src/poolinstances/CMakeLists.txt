include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_library(poolinstances STATIC
  fabric.cpp
  stratumMsg.cpp
  protocol.proto
)

protobuf_generate(TARGET poolinstances)

if (MSVC)
  set_source_files_properties("protocol.pb.cc" PROPERTIES COMPILE_FLAGS "/wd4244 /wd4267")
endif()

target_link_libraries(poolinstances ${TBB_IMPORTED_TARGETS})
add_dependencies(poolinstances mpir)
