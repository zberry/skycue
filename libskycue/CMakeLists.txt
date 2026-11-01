# libskycue: track types, the wire-protocol codec and the client.
# Usable on its own through add_subdirectory(): it finds its own dependencies
# when the enclosing tree has not.
#
# Options:
#   SKYCUE_INSTALL (ON): install and export libskycue. A consumer using
#     add_subdirectory() can turn it off to keep libskycue out of its own
#     install.

option(SKYCUE_INSTALL "Install libskycue" ON)

if(NOT TARGET Boost::system)
  find_package(Boost 1.66 REQUIRED COMPONENTS system)
endif()
if(NOT TARGET Threads::Threads)
  find_package(Threads REQUIRED)
endif()

add_library(skycue STATIC
  src/protocol.cpp
  src/client.cpp)
add_library(skycue::skycue ALIAS skycue)
# Static, but position-independent so it can be linked into a consumer's
# shared library (a plugin, say).
set_target_properties(skycue PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(skycue PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(skycue PUBLIC cxx_std_17)
target_link_libraries(skycue PUBLIC Boost::system Threads::Threads)

if(COMMAND skycue_set_warnings)
  skycue_set_warnings(skycue)
endif()

# Install and export, so consumers can use find_package(skycue) as well as
# add_subdirectory(). The version follows the skycue project when built from
# it; a consumer adding this directory directly gets the fallback.
if(NOT SKYCUE_INSTALL)
  return()
endif()

if(DEFINED skycue_VERSION)
  set(SKYCUE_LIB_VERSION "${skycue_VERSION}")
else()
  set(SKYCUE_LIB_VERSION 0.1.0)
endif()

include(CMakePackageConfigHelpers)

install(TARGETS skycue EXPORT skycueTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT skycueTargets NAMESPACE skycue:: DESTINATION lib/cmake/skycue)

configure_package_config_file(cmake/skycueConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/skycueConfig.cmake"
  INSTALL_DESTINATION lib/cmake/skycue)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/skycueConfigVersion.cmake"
  VERSION ${SKYCUE_LIB_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/skycueConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/skycueConfigVersion.cmake"
  DESTINATION lib/cmake/skycue)
