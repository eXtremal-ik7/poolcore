# command-line client
add_executable(poolrpccmd
  poolrpccmd.cpp
)

target_link_libraries(poolrpccmd
  poolcommon
  ${AIO_LIBRARY}
  ${P2P_LIBRARY}
  ${P2PUTILS_LIBRARY}
)
