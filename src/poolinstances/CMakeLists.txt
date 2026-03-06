include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_library(poolinstances STATIC
  fabric.cpp
)

add_dependencies(poolinstances poolcore)
target_include_directories(poolinstances PUBLIC ${RAPIDJSON_INCLUDE_DIRECTORY})

target_link_libraries(poolinstances
  blockmaker
  poolconfig
  TBB::tbb
  OpenSSL::SSL
  OpenSSL::Crypto
  RocksDB::rocksdb
  ${BIGNUM_LIBRARIES}
)
