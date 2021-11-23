include(FetchContent)

FetchContent_Declare(secp256k1
  GIT_REPOSITORY https://github.com/bitcoin-core/secp256k1
  GIT_TAG        39198a03eaa33d5902b16d3aefa7c441232f60fb
  SOURCE_DIR     ${CMAKE_SOURCE_DIR}/../dependencies/secp256k1
  PATCH_COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/secp256k1/CMakeLists.txt <SOURCE_DIR> 
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/secp256k1/libsecp256k1-config.h.cmake.in <SOURCE_DIR>/src
)

FetchContent_GetProperties(secp256k1)
if (NOT secp256k1_POPULATED)
  FetchContent_Populate(secp256k1)
  add_subdirectory(${secp256k1_SOURCE_DIR} ${secp256k1_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
