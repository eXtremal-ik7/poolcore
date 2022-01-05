include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_library(poolinstances STATIC
  fabric.cpp
  protocol.proto
)

target_include_directories(poolinstances PUBLIC ${RAPIDJSON_INCLUDE_DIRECTORY})

target_link_libraries(poolinstances
  TBB::tbb
  OpenSSL::SSL
  OpenSSL::Crypto
  RocksDB::rocksdb
  protobuf::libprotobuf
  ${BIGNUM_LIBRARIES}
)

protobuf_generate(TARGET poolinstances)

if (MSVC)
  set_source_files_properties("protocol.pb.cc" PROPERTIES COMPILE_FLAGS "/wd4244 /wd4267")
endif()
