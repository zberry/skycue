# A project that uses skycue::net on its own: nothing from the skycue tree but
# the net/ directory (SKYCUE_NET_DIR).
cmake_minimum_required(VERSION 3.25)
project(net_standalone CXX)

if(NOT SKYCUE_NET_DIR)
  message(FATAL_ERROR "pass -DSKYCUE_NET_DIR=<path to net/>")
endif()
set(SKYCUE_NET_BUILD_TESTS OFF CACHE BOOL "")
add_subdirectory(${SKYCUE_NET_DIR} net)

add_executable(net_standalone main.cpp)
target_link_libraries(net_standalone PRIVATE skycue::net)
