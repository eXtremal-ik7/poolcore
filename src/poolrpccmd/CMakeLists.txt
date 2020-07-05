# command-line client
add_executable(poolrpccmd
  poolrpccmd.cpp
)

target_link_libraries(poolrpccmd
  poolcommon
  loguru
  asyncio-0.5
  asyncioextras-0.5
  p2p
  p2putils
)

if (WIN32)
  target_link_libraries(poolrpccmd ws2_32 mswsock)
else()
  target_link_libraries(poolrpccmd dl)
endif()
