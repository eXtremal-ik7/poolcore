add_executable(noderpc
  noderpc.cpp
  ${GETOPT_SOURCES}
)

target_link_whole_archive(noderpc ${POOLFRONTEND_EXTRAS})
target_link_libraries(noderpc ${LIBRARIES})

add_executable(passwordhash
  passwordHash.cpp
)

target_link_libraries(passwordhash ${LIBRARIES})
