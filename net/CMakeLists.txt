# skycue_net: the small network library every skycue input and output uses.
# Usable on its own through add_subdirectory(): it finds its own dependencies
# when the enclosing tree has not.
#
# Options:
#   SKYCUE_NET_BUILD_TESTS: build the library tests (default: follows
#     SKYCUE_BUILD_TESTS, OFF when that is not defined).

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.25)
  project(skycue_net CXX)
  enable_testing()
endif()

if(NOT TARGET Boost::system)
  find_package(Boost 1.66 REQUIRED COMPONENTS system)
endif()
if(NOT TARGET Threads::Threads)
  find_package(Threads REQUIRED)
endif()

add_library(skycue_net STATIC
  src/backoff.cpp
  src/dialer.cpp
  src/endpoint.cpp
  src/framer.cpp
  src/log.cpp
  src/tcp_options.cpp)
add_library(skycue::net ALIAS skycue_net)
set_target_properties(skycue_net PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(skycue_net PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(skycue_net PUBLIC cxx_std_17)
target_link_libraries(skycue_net PUBLIC Boost::system Threads::Threads)

if(COMMAND skycue_set_warnings)
  skycue_set_warnings(skycue_net)
endif()

if(DEFINED SKYCUE_BUILD_TESTS)
  set(_skycue_net_tests_default ${SKYCUE_BUILD_TESTS})
else()
  set(_skycue_net_tests_default OFF)
endif()
option(SKYCUE_NET_BUILD_TESTS "Build the net library tests" ${_skycue_net_tests_default})
if(SKYCUE_NET_BUILD_TESTS)
  add_subdirectory(tests)
endif()
