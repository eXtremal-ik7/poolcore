add_executable(unittests
  main.cpp
  statsDataTests.cpp
)

target_link_libraries(unittests
  GTest::gtest
  ${LIBRARIES}
)
