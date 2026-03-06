add_executable(migrate
  migrate.cpp
  migratecommon.cpp
  priceHistory.cpp
  struct2.cpp
  v3.cpp
  ${GETOPT_SOURCES}
)

add_dependencies(migrate poolcore)
target_link_whole_archive(migrate ${POOLFRONTEND_EXTRAS})
target_link_libraries(migrate ${LIBRARIES})
