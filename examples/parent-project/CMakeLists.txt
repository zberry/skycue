# client_demo: a stand-in for a larger client program that vendors skycue/
# and builds libskycue with add_subdirectory(), or (CLIENT_DEMO_FIND_PACKAGE)
# uses an installed libskycue through find_package(). Not part of the skycue
# build: configure it on its own (see README.md).
# 3.25: skycue's own minimum, which configuring skycue/ needs anyway.
cmake_minimum_required(VERSION 3.25)
project(client_demo CXX)

# The parent's own settings. The standard is set here for the parent's
# directory (skycue's directory sets its own); PIC, visibility and warnings
# are set on client_demo only, so none of them reach skycue's targets.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(CLIENT_DEMO_WERROR "Treat warnings in client_demo's own code as errors" ON)
option(CLIENT_DEMO_FIND_PACKAGE
  "Use an installed libskycue (find_package(skycue), CMAKE_PREFIX_PATH at its prefix) instead of the vendored skycue/" OFF)

if(CLIENT_DEMO_FIND_PACKAGE)
  # Installed libskycue (a -dev/-devel package, or `cmake --install` of
  # skycue/): skycue::skycue with the same include path, cxx_std_17,
  # Boost::system and Threads::Threads as the vendored build gives.
  find_package(skycue CONFIG REQUIRED)
else()
  get_filename_component(_client_demo_default_skycue "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  set(SKYCUE_SOURCE_DIR "${_client_demo_default_skycue}" CACHE PATH
    "The skycue/ directory (in a real tree: the vendored copy)")
  if(NOT EXISTS "${SKYCUE_SOURCE_DIR}/libskycue/CMakeLists.txt")
    message(FATAL_ERROR "client_demo: SKYCUE_SOURCE_DIR=${SKYCUE_SOURCE_DIR} is not a skycue/ directory")
  endif()

  # libskycue only: no skycued, tools, web UI, tests or examples, and nothing
  # of skycue's in this project's `cmake --install`.
  set(SKYCUE_BUILD_DAEMON OFF)
  set(SKYCUE_BUILD_TESTS OFF)
  set(SKYCUE_BUILD_EXAMPLES OFF)
  set(SKYCUE_INSTALL OFF)
  add_subdirectory("${SKYCUE_SOURCE_DIR}" skycue)
endif()

add_executable(client_demo client_demo.cpp)
target_link_libraries(client_demo PRIVATE skycue::skycue)
set_target_properties(client_demo PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden)
target_compile_options(client_demo PRIVATE -Wall -Wextra)
if(CLIENT_DEMO_WERROR)
  target_compile_options(client_demo PRIVATE -Werror)
endif()
install(TARGETS client_demo RUNTIME DESTINATION bin)
