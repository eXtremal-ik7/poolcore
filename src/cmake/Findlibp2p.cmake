set(BUILD_DIR "${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_SYSTEM_NAME}")
set(PATHS
  ${ROOT_SOURCE_DIR}/libp2p/${BUILD_DIR}
)

find_library(AIO_LIBRARY asyncio-0.4
  PATHS ${PATHS}
  PATH_SUFFIXES asyncio
)

find_library(AIO_EXTRAS_LIBRARY asyncioextras-0.4
  PATHS ${PATHS}
  PATH_SUFFIXES asyncioextras
)

find_library(P2P_LIBRARY p2p
  PATHS ${PATHS}
  PATH_SUFFIXES p2p
)

find_library(P2PUTILS_LIBRARY p2putils
  PATHS ${PATHS}
  PATH_SUFFIXES p2putils
)

find_path(AIO_INCLUDE_DIR "asyncio/asyncio.h"
  PATH ${ROOT_SOURCE_DIR}/libp2p/src/include
)

find_path(AIO_INCLUDE_DIR2 "config.h"
  PATHS ${PATHS}
  PATH_SUFFIXES include
)

set(AIO_INCLUDE_DIR ${AIO_INCLUDE_DIR} ${AIO_INCLUDE_DIR2})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(AIO_LIBRARY ${AIO_LIBRARY} rt)
endif()
