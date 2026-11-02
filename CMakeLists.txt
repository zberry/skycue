cmake_minimum_required(VERSION 3.26)
project(skycue VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SKYCUE_BUILD_TESTS "Build the skycue unit tests" ON)
option(SKYCUE_BUILD_EXAMPLES "Build the example libskycue consumers" ON)

find_package(Boost 1.66 REQUIRED COMPONENTS system program_options unit_test_framework)
find_package(Threads REQUIRED)

include(GNUInstallDirs)
include(cmake/CompilerWarnings.cmake)

enable_testing()

add_subdirectory(libskycue)
add_subdirectory(skycued)
add_subdirectory(skycue-dump)
add_subdirectory(skycue-rawrec)
add_subdirectory(skycue-rawplay)
add_subdirectory(skycue-stats)
if(SKYCUE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(SKYCUE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
