add_executable(unittests
  main.cpp
  statsDataTests.cpp
  taggedTests.cpp
)

target_link_libraries(unittests
  GTest::gtest
  ${LIBRARIES}
)
