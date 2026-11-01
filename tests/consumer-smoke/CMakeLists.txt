# A minimal libskycue consumer, built by consumer_smoke.sh in both supported
# ways:
#   -DSKYCUE_CONSUMER_MODE=subdir  -DSKYCUE_LIB_DIR=<skycue/libskycue>
#   -DSKYCUE_CONSUMER_MODE=package (with CMAKE_PREFIX_PATH at an install)
cmake_minimum_required(VERSION 3.16)
project(skycue_consumer_smoke LANGUAGES CXX)

set(SKYCUE_CONSUMER_MODE "package" CACHE STRING "How to obtain libskycue: subdir or package")

if(SKYCUE_CONSUMER_MODE STREQUAL "subdir")
  if(NOT SKYCUE_LIB_DIR)
    message(FATAL_ERROR "subdir mode needs -DSKYCUE_LIB_DIR=<path to skycue/libskycue>")
  endif()
  add_subdirectory(${SKYCUE_LIB_DIR} skycue-lib)
elseif(SKYCUE_CONSUMER_MODE STREQUAL "package")
  find_package(skycue REQUIRED)
else()
  message(FATAL_ERROR "SKYCUE_CONSUMER_MODE must be subdir or package, not '${SKYCUE_CONSUMER_MODE}'")
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE skycue::skycue)
