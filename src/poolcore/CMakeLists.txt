add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bitcoinRpc.idl.h ${CMAKE_CURRENT_BINARY_DIR}/bitcoinRpc.idl.cpp
  COMMAND idltool ${CMAKE_CURRENT_SOURCE_DIR}/bitcoinRpc.idl -o ${CMAKE_CURRENT_BINARY_DIR}/bitcoinRpc.idl.h -I ${CMAKE_SOURCE_DIR} --struct-prefix C --pascal-case
  DEPENDS idltool ${CMAKE_CURRENT_SOURCE_DIR}/bitcoinRpc.idl ${CMAKE_SOURCE_DIR}/poolcommon/hash.idl
  COMMENT "Generating bitcoinRpc.idl.h and bitcoinRpc.idl.cpp"
)
add_custom_target(generate_bitcoinRpc_idl DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bitcoinRpc.idl.h ${CMAKE_CURRENT_BINARY_DIR}/bitcoinRpc.idl.cpp)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ethereumRpc.idl.h ${CMAKE_CURRENT_BINARY_DIR}/ethereumRpc.idl.cpp
  COMMAND idltool ${CMAKE_CURRENT_SOURCE_DIR}/ethereumRpc.idl -o ${CMAKE_CURRENT_BINARY_DIR}/ethereumRpc.idl.h -I ${CMAKE_SOURCE_DIR} --struct-prefix C --pascal-case
  DEPENDS idltool ${CMAKE_CURRENT_SOURCE_DIR}/ethereumRpc.idl ${CMAKE_SOURCE_DIR}/poolcommon/hash.idl
  COMMENT "Generating ethereumRpc.idl.h and ethereumRpc.idl.cpp"
)
add_custom_target(generate_ethereumRpc_idl DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ethereumRpc.idl.h ${CMAKE_CURRENT_BINARY_DIR}/ethereumRpc.idl.cpp)

# Backend library
add_library(poolcore STATIC
  bitcoinRpcClient.cpp
  ethereumRpcClient.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/bitcoinRpc.idl.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/ethereumRpc.idl.cpp

  accounting.cpp
  accountingApi.cpp
  accountingData.cpp
  accountingPayouts.cpp
  accountingState.cpp
  backend.cpp
  backendData.cpp
  dbFormat.cpp
  base58.cpp
  clientDispatcher.cpp
  feeEstimator.cpp
  kvdb.cpp
  poolCore.cpp
  poolInstance.cpp
  priceFetcher.cpp
  rocksdbBase.cpp
  shareAccumulator.cpp
  shareLog.cpp
  statsData.cpp
  statsMergeOperator.cpp
  statistics.cpp
  thread.cpp
  usermgr.cpp
  workSummary.cpp
)

add_dependencies(poolcore poolcommon generate_bitcoinRpc_idl generate_ethereumRpc_idl generate_hash_idl)
target_include_directories(poolcore PUBLIC ${RAPIDJSON_INCLUDE_DIRECTORY} ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(poolcore
  TBB::tbb
  OpenSSL::SSL
  OpenSSL::Crypto
  RocksDB::rocksdb
)
