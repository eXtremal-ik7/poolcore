# Backend library
add_library(poolcore STATIC
  base58.cpp
  api.cpp
  backendData.cpp
  bitcoinRpcClient.cpp
  kvdb.cpp
  rocksdbBase.cpp
  backend.cpp
  accounting.cpp
  statistics.cpp
  usermgr.cpp
  poolCore.cpp
)

target_link_libraries(poolcore poolcommon RocksDB::rocksdb ${TBB_IMPORTED_TARGETS} OpenSSL::SSL OpenSSL::Crypto)
