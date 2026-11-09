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

# net/ also builds as a copy of this directory alone: then the shared module
# (../cmake/BoostSystem.cmake) is absent and the same lookup is repeated here.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../cmake/BoostSystem.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/../cmake/BoostSystem.cmake")
  skycue_require_boost_system(1.66)
elseif(NOT TARGET Boost::system)
  find_package(Boost 1.66 REQUIRED)
  find_package(Boost 1.66 QUIET COMPONENTS system)
  if(NOT TARGET Boost::system)
    # Header-only since Boost 1.69; some distributions ship no boost_system package.
    # Keep this in step with ../cmake/BoostSystem.cmake.
    if(DEFINED Boost_VERSION_STRING AND Boost_VERSION_STRING VERSION_LESS 1.69)
      message(FATAL_ERROR "skycue: Boost ${Boost_VERSION_STRING} needs the compiled Boost.System library "
        "(install the boost system development package)")
    endif()
    add_library(Boost::system INTERFACE IMPORTED GLOBAL)
    if(TARGET Boost::headers)
      target_link_libraries(Boost::system INTERFACE Boost::headers)
    else()
      target_link_libraries(Boost::system INTERFACE Boost::boost)
    endif()
  endif()
endif()
if(NOT TARGET Threads::Threads)
  find_package(Threads REQUIRED)
endif()

add_library(skycue_net STATIC
  src/backoff.cpp
  src/dialer.cpp
  src/endpoint.cpp
  src/fanout.cpp
  src/framer.cpp
  src/listener.cpp
  src/log.cpp
  src/tcp_options.cpp
  src/udp_receiver.cpp
  src/udp_sender.cpp
  src/write_queue.cpp)
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
