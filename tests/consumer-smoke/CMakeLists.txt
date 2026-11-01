# A minimal libskycue consumer, built by consumer_smoke.sh in each supported
# way:
#   -DSKYCUE_CONSUMER_MODE=subdir  -DSKYCUE_LIB_DIR=<skycue/libskycue>
#   -DSKYCUE_CONSUMER_MODE=package (with CMAKE_PREFIX_PATH at an install)
#   -DSKYCUE_CONSUMER_MODE=shared  -DSKYCUE_LIB_DIR=<skycue/libskycue>: as
#     subdir with SKYCUE_INSTALL=OFF, linking libskycue into a SHARED library
#     (which needs it to be position-independent) that the program uses.
cmake_minimum_required(VERSION 3.16)
project(skycue_consumer_smoke LANGUAGES CXX)

set(SKYCUE_CONSUMER_MODE "package" CACHE STRING "How to obtain libskycue: subdir or package")

if(SKYCUE_CONSUMER_MODE STREQUAL "subdir" OR SKYCUE_CONSUMER_MODE STREQUAL "shared")
  if(NOT SKYCUE_LIB_DIR)
    message(FATAL_ERROR "subdir mode needs -DSKYCUE_LIB_DIR=<path to skycue/libskycue>")
  endif()
  add_subdirectory(${SKYCUE_LIB_DIR} skycue-lib)
elseif(SKYCUE_CONSUMER_MODE STREQUAL "package")
  find_package(skycue REQUIRED)
else()
  message(FATAL_ERROR "SKYCUE_CONSUMER_MODE must be subdir, package or shared, not '${SKYCUE_CONSUMER_MODE}'")
endif()

if(SKYCUE_CONSUMER_MODE STREQUAL "shared")
  add_library(consumer_plugin SHARED plugin.cpp)
  target_link_libraries(consumer_plugin PRIVATE skycue::skycue)
  add_executable(consumer plugin_main.cpp)
  target_link_libraries(consumer PRIVATE consumer_plugin)
  install(TARGETS consumer consumer_plugin RUNTIME DESTINATION bin LIBRARY DESTINATION lib)
else()
  add_executable(consumer main.cpp)
  target_link_libraries(consumer PRIVATE skycue::skycue)
endif()
