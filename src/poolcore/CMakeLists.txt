# Backend library
add_library(poolcore STATIC
  base58.cpp
  api.cpp
  backendData.cpp
  kvdb.cpp
  leveldbBase.cpp
  backend.cpp
  accounting.cpp
  statistics.cpp
)

target_link_libraries(poolcore poolcommon)