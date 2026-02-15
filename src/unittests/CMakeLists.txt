add_executable(unittests
  main.cpp
)

target_link_libraries(unittests
  GTest::gtest
  ${LIBRARIES}
)
