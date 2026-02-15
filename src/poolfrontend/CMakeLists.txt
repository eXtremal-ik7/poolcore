add_executable(poolfrontend
  config.cpp
  main.cpp
  http.cpp
  ${GETOPT_SOURCES}
)

target_include_directories(poolfrontend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_whole_archive(poolfrontend ${POOLFRONTEND_EXTRAS})
target_link_libraries(poolfrontend poolinstances ${LIBRARIES})
