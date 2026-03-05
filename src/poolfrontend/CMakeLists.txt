add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/poolapi.idl.h ${CMAKE_CURRENT_BINARY_DIR}/poolapi.idl.cpp
  COMMAND idltool ${CMAKE_CURRENT_SOURCE_DIR}/poolapi.idl -I ${CMAKE_CURRENT_SOURCE_DIR}/.. -o ${CMAKE_CURRENT_BINARY_DIR}/poolapi.idl.h --struct-prefix C --pascal-case
  DEPENDS idltool ${CMAKE_CURRENT_SOURCE_DIR}/poolapi.idl ${CMAKE_CURRENT_SOURCE_DIR}/../poolcommon/types.idl
  COMMENT "Generating poolapi.idl.h and poolapi.idl.cpp"
)
add_custom_target(generate_poolapi_idl DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/poolapi.idl.h ${CMAKE_CURRENT_BINARY_DIR}/poolapi.idl.cpp)

add_executable(poolfrontend
  main.cpp
  http.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/poolapi.idl.cpp
  ${GETOPT_SOURCES}
)
add_dependencies(poolfrontend generate_poolapi_idl)
target_include_directories(poolfrontend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_whole_archive(poolfrontend ${POOLFRONTEND_EXTRAS})
target_link_libraries(poolfrontend poolinstances ${LIBRARIES})
