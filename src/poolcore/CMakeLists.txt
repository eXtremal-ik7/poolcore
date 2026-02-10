# Backend library
add_library(poolcore STATIC
  bitcoinRpcClient.cpp
  ethereumRpcClient.cpp

  accounting.cpp
  backend.cpp
  backendData.cpp
  dbFormat.cpp
  base58.cpp
  clientDispatcher.cpp
  kvdb.cpp
  poolCore.cpp
  poolInstance.cpp
  priceFetcher.cpp
  rocksdbBase.cpp
  shareLog.cpp
  statsMergeOperator.cpp
  statistics.cpp
  thread.cpp
  usermgr.cpp
)

target_include_directories(poolcore PUBLIC ${RAPIDJSON_INCLUDE_DIRECTORY})

target_link_libraries(poolcore
  TBB::tbb
  OpenSSL::SSL
  OpenSSL::Crypto
  RocksDB::rocksdb
)
