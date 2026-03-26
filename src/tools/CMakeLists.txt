add_executable(noderpc
  noderpc.cpp
  ${GETOPT_SOURCES}
)

add_dependencies(noderpc poolcore)
target_link_whole_archive(noderpc ${POOLFRONTEND_EXTRAS})
target_link_libraries(noderpc ${LIBRARIES})

add_executable(addressgen
  addressgen.cpp
  ${GETOPT_SOURCES}
)

add_dependencies(addressgen poolcore)
target_link_whole_archive(addressgen ${POOLFRONTEND_EXTRAS})
target_link_libraries(addressgen ${LIBRARIES})

add_executable(passwordhash
  passwordHash.cpp
)

add_dependencies(passwordhash poolcore)
target_link_libraries(passwordhash ${LIBRARIES})
