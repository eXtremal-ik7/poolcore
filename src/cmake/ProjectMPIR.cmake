include(ExternalProject)

set(MPIR_DIRECTORY "${CMAKE_SOURCE_DIR}/../dependencies/mpir")
set(MPIR_INSTALL_DIRECTORY "${CMAKE_SOURCE_DIR}/../dependencies/install")

if (NOT MSVC)
  ExternalProject_Add(mpir
    GIT_REPOSITORY https://github.com/BrianGladman/mpir
    GIT_TAG 800bbce9f1dc17f4cffa046dbc5c230291bc974b
    GIT_SHALLOW 1
    SOURCE_DIR ${MPIR_DIRECTORY}
    BINARY_DIR ${MPIR_DIRECTORY}
    CONFIGURE_COMMAND sh ./autogen.sh && sh ./configure --enable-cxx --disable-static --enable-shared --prefix=${MPIR_INSTALL_DIRECTORY}
    BUILD_COMMAND ${CMAKE_MAKE_PROGRAM} -j
    INSTALL_COMMAND ${CMAKE_MAKE_PROGRAM} install
    UPDATE_COMMAND ""
  )

  add_library(MPIR_LIBRARY__     SHARED IMPORTED DEPENDS mpir)
  add_library(MPIR_CXX_LIBRARY__ SHARED IMPORTED DEPENDS mpir)
  set_target_properties(MPIR_LIBRARY__     PROPERTIES IMPORTED_LOCATION ${MPIR_INSTALL_DIRECTORY}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}mpir${CMAKE_SHARED_LIBRARY_SUFFIX})
  set_target_properties(MPIR_CXX_LIBRARY__ PROPERTIES IMPORTED_LOCATION ${MPIR_INSTALL_DIRECTORY}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}mpirxx${CMAKE_SHARED_LIBRARY_SUFFIX})
  set(MPIR_INCLUDE_DIR ${MPIR_INSTALL_DIRECTORY}/include)
  set(MPIR_LIBRARIES MPIR_LIBRARY__ MPIR_CXX_LIBRARY__)
else()
  # MPIR have separate solution files for each visual studio version
  # Visual Studio 2019 Dev16  15.0  1920-1929
  # Visual Studio 2017 Dev15  15.0  1910-1919
  # Visual Studio 2015 Dev14  14.0  1900
  # Visual Studio 2013 Dev12  12.0  1800
  if (${MSVC_VERSION} EQUAL 1800)
    set(SOLUTION_DIR vs13)
  elseif (${MSVC_VERSION} EQUAL 1900)
    set(SOLUTION_DIR vs15)
  elseif (${MSVC_VERSION} GREATER_EQUAL 1910 AND ${MSVC_VERSION} LESS_EQUAL 1919)
    set(SOLUTION_DIR vs17)
  elseif (${MSVC_VERSION} GREATER_EQUAL 1920 AND ${MSVC_VERSION} LESS_EQUAL 1929)
    set(SOLUTION_DIR vs19)
  else()
    message(ERROR "Unsupported Visual Studio version")
  endif()

  # Patch sources: use Multithreaded DLL runtime library
  ExternalProject_Add(mpir
    GIT_REPOSITORY https://github.com/BrianGladman/mpir
    GIT_TAG master
    GIT_SHALLOW 1
    SOURCE_DIR ${MPIR_DIRECTORY}
    BINARY_DIR ${MPIR_DIRECTORY}/msvc/${SOLUTION_DIR}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND msbuild.bat gc DLL ${CMAKE_VS_PLATFORM_NAME} Release
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
  )

  add_library(MPIR_LIBRARY__ SHARED IMPORTED DEPENDS mpir)
  set_target_properties(MPIR_LIBRARY__ PROPERTIES IMPORTED_LOCATION ${MPIR_DIRECTORY}/dll/${CMAKE_VS_PLATFORM_NAME}/Release/mpir.dll)
  set_target_properties(MPIR_LIBRARY__ PROPERTIES IMPORTED_IMPLIB ${MPIR_DIRECTORY}/dll/${CMAKE_VS_PLATFORM_NAME}/Release/mpir.lib)
  set(MPIR_INCLUDE_DIR ${MPIR_DIRECTORY}/dll/${CMAKE_VS_PLATFORM_NAME}/Release)
  set(MPIR_LIBRARIES MPIR_LIBRARY__)
endif()
