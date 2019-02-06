cmake_minimum_required(VERSION 3.0)

option(BTC_ENABLED "Build bitcoin core client integration" OFF)
option(DTC_ENABLED "Build datacoin client integration" OFF)
option(XPM_ENABLED "Build primecoin/XPM client integration" OFF)
option(ZCASH_ENABLED "Build ZCash/ZEC client integration" OFF)

set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH}
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake
)

find_package(Boost COMPONENTS thread system REQUIRED)
find_package(FlatBuffers REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(libp2p REQUIRED)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  add_definitions(-fPIC)
endif ()

include_directories(
  poolcommon
  ${AIO_INCLUDE_DIR}
  ${BOOST_INCLUDEDIR}
  ${FLATBUFFERS_INCLUDE_DIR}
  ${OPENSSL_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}
)

add_subdirectory(poolcommon)
add_subdirectory(poolcore)
add_subdirectory(poolrpccmd)

if (BTC_ENABLED)
  add_subdirectory(btc)
endif()

if (DTC_ENABLED)
  add_subdirectory(dtc)
endif()

if (XPM_ENABLED)
  add_subdirectory(xpm)
endif()

if (ZCASH_ENABLED)
  add_subdirectory(zcash)
endif()

