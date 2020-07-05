include(FetchContent)

set(ZMTP_ENABLED ON CACHE INTERNAL "")

FetchContent_Declare(
  libp2p
  GIT_REPOSITORY https://github.com/eXtremal-ik7/libp2p.git
  GIT_TAG        master
  GIT_SHALLOW    1
  SOURCE_DIR     ${CMAKE_SOURCE_DIR}/../dependencies/libp2p
)

FetchContent_GetProperties(libp2p)
if (NOT libp2p_POPULATED)
  FetchContent_Populate(libp2p)
  add_subdirectory(${libp2p_SOURCE_DIR}/src ${libp2p_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
