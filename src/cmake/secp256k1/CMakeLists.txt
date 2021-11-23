cmake_minimum_required(VERSION 3.10)
project(secp256k1 LANGUAGES C)

set(VERSION "0.1")

include(CheckIncludeFile)
include(CheckTypeSize)

option(ENABLE_GMP "Build with GNU MP library" OFF)

option(AC_APPLE_UNIVERSAL_BUILD "Define if building universal (internal helper macro)" OFF)
option(COVERAGE "Coverage" OFF)
option(ENABLE_ASM "Enable assembly optimizations" OFF)
option(ENABLE_MODULE_ECDH "Enable ECDH shared secret computation (experimental)" OFF)
option(ENABLE_MODULE_RECOVERY "Enable ECDSA pubkey recovery module" OFF)
option(USE_ECMULT_STATIC_PRECOMPUTATION "Enable precomputed ecmult table for signing" ON)
option(USE_ENDOMORPHISM "Define this symbol to use endomorphism optimization" OFF)
option(USE_EXTERNAL_DEFAULT_CALLBACKS "Define this symbol if an external implementation of the default callbacks is used" OFF)

# Test & benchmarks
option(ENABLE_TESTS "Enable tests" OFF)
option(ENABLE_OPENSSL_TESTS "Enable OpenSSL tests" OFF)

set(ECMULT_GEN_PREC_BITS 4 CACHE STRING "Set ecmult gen precision bits" FORCE)
set(ECMULT_WINDOW_SIZE 15 CACHE STRING "Set window size for ecmult precomputation" FORCE)

set(PACKAGE "libsecp256k1" CACHE STRING "" FORCE)
set(PACKAGE_BUGREPORT "" CACHE STRING "" FORCE)
set(PACKAGE_NAME "libsecp256k1" CACHE STRING "" FORCE)
set(PACKAGE_STRING "libsecp256k1 0.1" CACHE STRING "" FORCE)
set(PACKAGE_TARNAME "libsecp256k1" CACHE STRING "" FORCE)
set(PACKAGE_URL "libsecp256k1" CACHE STRING "" FORCE)
set(PACKAGE_VERSION ${VERSION} CACHE STRING "" FORCE)
set(LT_OBJDIR ".libs" CACHE STRING "" FORCE)

CHECK_INCLUDE_FILE(dlfcn.h HAVE_DLFCN_H)
CHECK_INCLUDE_FILE(float.h HAVE_FLOAT_H)
CHECK_INCLUDE_FILE(inttypes.h HAVE_INTTYPES_H)
CHECK_INCLUDE_FILE(memory.h HAVE_MEMORY_H)
CHECK_INCLUDE_FILE(stdarg.h HAVE_STDARG_H)
CHECK_INCLUDE_FILE(stdint.h HAVE_STDINT_H)
CHECK_INCLUDE_FILE(stdlib.h HAVE_STDLIB_H)
CHECK_INCLUDE_FILE(strings.h HAVE_STRINGS_H)
CHECK_INCLUDE_FILE(string.h HAVE_STRING_H)
CHECK_INCLUDE_FILE(sys/stat.h HAVE_SYS_STAT_H)
CHECK_INCLUDE_FILE(sys/types.h HAVE_SYS_TYPES_H)
CHECK_INCLUDE_FILE(unistd.h HAVE_UNISTD_H)

if (HAVE_STDLIB_H AND HAVE_STDARG_H AND HAVE_STRING_H AND HAVE_FLOAT_H)
  set(STDC_HEADERS TRUE)
endif()

CHECK_TYPE_SIZE(__int128 HAVE___INT128)

if (ENABLE_ASM)
  if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64" OR ${CMAKE_SYSTEM_PROCESSOR STREQUAL} "AMD64")
    set(USE_ASM_X86_64 "1")
  elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "arm")
    set(USE_EXTERNAL_ASM "1")
  else()
    message(ERROR "invalid assembly optimizations")
  endif()
endif()

# Microsort visual studio doesn't support __int128
if("${CMAKE_SIZEOF_VOID_P}" EQUAL "8" AND NOT MSVC)
  # 64-bit target
  set(USE_FIELD_10X26 "0")
  set(USE_FIELD_5X52 "1")
  set(USE_SCALAR_4X64 "1")
  set(USE_SCALAR_8X32 "0")  
else()
  # 32-bit target
  set(USE_FIELD_10X26 "1")
  set(USE_FIELD_5X52 "0")
  set(USE_SCALAR_4X64 "0")
  set(USE_SCALAR_8X32 "1")  
endif()

if (ENABLE_OPENSSL_TESTS)
  find_package(OpenSSL REQUIRED)
  if (OpenSSL_FOUND)
    set(HAVE_LIBCRYPTO 1)
  endif()
endif()

if (ENABLE_GMP)
  find_library(GMP_LIBRARY gmp)
  if (GMP_LIBRARY)
    set(HAVE_LIBGMP 1)
    set(USE_NUM_GMP 1)
    set(USE_FIELD_INV_NUM 1)
    set(USE_SCALAR_INV_NUM 1)
  endif()
else()
  set(USE_NUM_NONE "1")
  set(USE_FIELD_INV_BUILTIN "1")
  set(USE_SCALAR_INV_BUILTIN "1")
endif()


configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/libsecp256k1-config.h.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/src/libsecp256k1-config.h
)


include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/src)

if (NOT MSVC)
  set(GEN_CONTEXT_COMPILE_OPTIONS -Wall -Wextra -Wno-unused-function)
  set(DEFAULT_COMPILE_OPTIONS -DHAVE_CONFIG_H -DSECP256K1_BUILD -std=c89 -pedantic -Wall -Wextra -Wcast-align -Wnested-externs -Wshadow -Wstrict-prototypes -Wno-unused-function -Wno-long-long -Wno-overlength-strings)
else()
  set(DEFAULT_COMPILE_OPTIONS -DHAVE_CONFIG_H -DSECP256K1_BUILD)
endif()

# Build gen_context
add_executable(gen_context ${CMAKE_CURRENT_SOURCE_DIR}/src/gen_context.c)
target_compile_options(gen_context PRIVATE ${GEN_CONTEXT_COMPILE_OPTIONS})

# Run gen_context
if (USE_ECMULT_STATIC_PRECOMPUTATION)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/src)
  add_custom_target(ecmult_static_context gen_context WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# Main static library
add_library(secp256k1 STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/secp256k1.c)
if (NOT MSVC)
  target_compile_definitions(secp256k1 PRIVATE -DVALGRIND)
endif()
target_compile_options(secp256k1 PRIVATE ${DEFAULT_COMPILE_OPTIONS})
add_dependencies(secp256k1 ecmult_static_context)

# Tests
if (ENABLE_TESTS)
  add_executable(tests ${CMAKE_CURRENT_SOURCE_DIR}/src/tests.c)
  target_include_directories(tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  if (NOT MSVC)
    target_compile_definitions(tests PRIVATE -DVALGRIND)
  endif()
  target_compile_definitions(tests PRIVATE -DVERIFY)
  target_compile_options(tests PRIVATE ${DEFAULT_COMPILE_OPTIONS})
  target_link_libraries(tests secp256k1)
    
  if (NOT MSVC)
    add_executable(valgrind_ctime_test ${CMAKE_CURRENT_SOURCE_DIR}/src/valgrind_ctime_test.c)
    target_compile_options(valgrind_ctime_test PRIVATE ${DEFAULT_COMPILE_OPTIONS})
    target_link_libraries(valgrind_ctime_test secp256k1)
  endif()
  
  add_executable(exhaustive_tests ${CMAKE_CURRENT_SOURCE_DIR}/src/tests_exhaustive.c)
  target_compile_definitions(exhaustive_tests PRIVATE -DVERIFY)
  target_compile_options(exhaustive_tests PRIVATE ${DEFAULT_COMPILE_OPTIONS})
  target_link_libraries(exhaustive_tests secp256k1)
endif()
