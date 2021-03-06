include("cmake/HunterGate.cmake")
HunterGate(
    URL "https://github.com/eXtremal-ik7/hunter/archive/v0.23.260-with-fb.zip"
    SHA1 "6b07ca070eae553de0ae4d160a8f916983cee468"
)

cmake_minimum_required(VERSION 3.11)
project(poolcore)
set (CMAKE_CXX_STANDARD 17)

if (SANITIZER_ENABLED)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
endif()

set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH}
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake
)

set(CMAKE_DEBUG_POSTFIX d)
add_definitions(-DTBB_USE_EXCEPTIONS=0)

if(MSVC)
  add_definitions(
    -D_CRT_SECURE_NO_DEPRECATE
    -D_CRT_SECURE_NO_WARNINGS
    -D_CRT_NONSTDC_NO_DEPRECATE
    -D__STDC_LIMIT_MACROS
    -D__STDC_FORMAT_MACROS
    -DNOMINMAX
    -DNOGDI
  )

  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GR-")
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/msvc/include)
else(MSVC)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -fno-rtti")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcx16")
endif(MSVC)

set(protobuf_MODULE_COMPATIBLE TRUE)

set(_tbb_cmake_module_path ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(${_tbb_cmake_module_path}/TBBGet.cmake)
include(${_tbb_cmake_module_path}/TBBBuild.cmake)
tbb_get(TBB_ROOT tbb_root SOURCE_CODE RELEASE_TAG v2020.3)
tbb_build(TBB_ROOT ${tbb_root} CONFIG_DIR TBB_DIR)
find_package(TBB REQUIRED tbb)

hunter_add_package(libsodium)
hunter_add_package(OpenSSL)
hunter_add_package(rocksdb)
hunter_add_package(RapidJSON)
hunter_add_package(Protobuf)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProjectLibp2p.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProjectMPIR.cmake)

add_definitions(-DGOOGLE_PROTOBUF_NO_RTTI)
find_package(libsodium CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(RocksDB CONFIG REQUIRED)
find_package(RapidJSON CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  add_definitions(-fPIC)
endif ()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

include_directories(
  ${MPIR_INCLUDE_DIR}
  ${OPENSSL_INCLUDE_DIR}
  ${libp2p_SOURCE_DIR}/src/include
  ${libp2p_BINARY_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/include
)

# Logging library
add_library(loguru STATIC ${CMAKE_CURRENT_SOURCE_DIR}/loguru/loguru.cpp)

add_subdirectory(blockmaker)
add_subdirectory(poolcommon)
add_subdirectory(poolcore)
add_subdirectory(poolinstances)

