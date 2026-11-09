# libskycue: track types, the wire-protocol codec and the client.
# Usable on its own through add_subdirectory(): it finds its own dependencies
# when the enclosing tree has not.
#
# Options:
#   SKYCUE_INSTALL (ON): install and export libskycue. A consumer using
#     add_subdirectory() can turn it off to keep libskycue out of its own
#     install.

option(SKYCUE_INSTALL "Install libskycue" ON)

# Install directories: lib, lib64 or lib/<multiarch> as the platform has it
# (a parent build may pass CMAKE_INSTALL_LIBDIR=lib). Included
# here as well as in ../CMakeLists.txt so a consumer adding this directory
# directly gets them too.
include(GNUInstallDirs)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/BoostSystem.cmake")
skycue_require_boost_system(1.66)
if(NOT TARGET Threads::Threads)
  find_package(Threads REQUIRED)
endif()

# libskycue is deliberately standalone: it links only Boost.System and Threads,
# so a consumer ships nothing else. Its socket code (connect with resolve,
# listen, UDP bind, keep-alive, reconnect timer) therefore duplicates part of
# net/, the daemon's skycue::net library; the duplication is accepted.
add_library(skycue STATIC
  src/protocol.cpp
  src/client.cpp
  src/ecef.cpp)
add_library(skycue::skycue ALIAS skycue)
# Static, but position-independent so it can be linked into a consumer's
# shared library (a plugin, say).
set_target_properties(skycue PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The version follows the skycue project when built from it; a consumer
# adding this directory directly reads it from project() in
# ../CMakeLists.txt, the one place it is written. Done before the
# SKYCUE_INSTALL return below so <skycue/version.hpp> exists either way.
if(DEFINED skycue_VERSION)
  set(SKYCUE_LIB_VERSION "${skycue_VERSION}")
else()
  file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt" _skycue_project_line
    REGEX "^project\\(skycue VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt")
  if(NOT _skycue_project_line MATCHES "^project\\(skycue VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)")
    message(FATAL_ERROR "libskycue: no project(skycue VERSION x.y.z) in ${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt")
  endif()
  set(SKYCUE_LIB_VERSION "${CMAKE_MATCH_1}")
endif()
configure_file(cmake/version.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/include/skycue/version.hpp" @ONLY)

target_include_directories(skycue PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(skycue PUBLIC cxx_std_17)
target_link_libraries(skycue PUBLIC Boost::system Threads::Threads)

if(COMMAND skycue_set_warnings)
  skycue_set_warnings(skycue)
endif()

# Install and export, so consumers can use find_package(skycue) as well as
# add_subdirectory().
if(NOT SKYCUE_INSTALL)
  return()
endif()

include(CMakePackageConfigHelpers)

install(TARGETS skycue EXPORT skycueTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/skycue/version.hpp"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/skycue)
install(EXPORT skycueTargets NAMESPACE skycue:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/skycue)

configure_package_config_file(cmake/skycueConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/skycueConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/skycue)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/skycueConfigVersion.cmake"
  VERSION ${SKYCUE_LIB_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/skycueConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/skycueConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/skycue)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/BoostSystem.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/skycue RENAME skycueBoostSystem.cmake)
