# command-line client
add_executable(poolrpccmd
  poolrpccmd.cpp
)

target_link_libraries(poolrpccmd
  poolcommon
  loguru
  ${AIO_LIBRARY}
  ${P2P_LIBRARY}
  ${P2PUTILS_LIBRARY}
)

if (WIN32)
  target_link_libraries(poolrpccmd ws2_32 mswsock)
else()
  target_link_libraries(poolrpccmd dl)
endif()
