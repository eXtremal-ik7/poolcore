# Backend library
add_library(poolcore STATIC
  accounting.cpp
  api.cpp
  backend.cpp
  backendData.cpp
  base58.cpp
  bitcoinRpcClient.cpp
  clientDispatcher.cpp
  kvdb.cpp
  poolCore.cpp
  poolInstance.cpp
  rocksdbBase.cpp
  statistics.cpp
  thread.cpp
  usermgr.cpp
)

target_link_libraries(poolcore poolcommon blockmaker RocksDB::rocksdb ${TBB_IMPORTED_TARGETS} OpenSSL::SSL OpenSSL::Crypto RapidJSON::rapidjson)
