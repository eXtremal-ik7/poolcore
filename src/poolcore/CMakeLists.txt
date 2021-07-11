# Backend library
add_library(poolcore STATIC
  bitcoinRpcClient.cpp
  ethereumRpcClient.cpp

  accounting.cpp
  backend.cpp
  backendData.cpp
  base58.cpp
  clientDispatcher.cpp
  kvdb.cpp
  poolCore.cpp
  poolInstance.cpp
  priceFetcher.cpp
  rocksdbBase.cpp
  shareLog.cpp
  statistics.cpp
  thread.cpp
  usermgr.cpp
)

target_link_libraries(poolcore ${TBB_IMPORTED_TARGETS})
