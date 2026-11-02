# client_demo: a stand-in for a larger client program that vendors skycue/
# and builds libskycue with add_subdirectory(). Not part of the skycue build:
# configure it on its own (see README.md).
cmake_minimum_required(VERSION 3.20)
project(client_demo CXX)

# The parent's own settings. They apply to its targets and are what a real
# parent would set; skycue's directory sets its own standard and warnings.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(CLIENT_DEMO_WERROR "Treat warnings in client_demo's own code as errors" ON)

get_filename_component(_client_demo_default_skycue "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SKYCUE_SOURCE_DIR "${_client_demo_default_skycue}" CACHE PATH
  "The skycue/ directory (in a real tree: the vendored copy)")
if(NOT EXISTS "${SKYCUE_SOURCE_DIR}/libskycue/CMakeLists.txt")
  message(FATAL_ERROR "client_demo: SKYCUE_SOURCE_DIR=${SKYCUE_SOURCE_DIR} is not a skycue/ directory")
endif()

# libskycue only: no skycued, tools, web UI, tests or examples, and nothing of
# skycue's in this project's `cmake --install`.
set(SKYCUE_BUILD_DAEMON OFF)
set(SKYCUE_BUILD_TESTS OFF)
set(SKYCUE_BUILD_EXAMPLES OFF)
set(SKYCUE_INSTALL OFF)
add_subdirectory("${SKYCUE_SOURCE_DIR}" skycue)

add_executable(client_demo client_demo.cpp)
target_link_libraries(client_demo PRIVATE skycue::skycue)
target_compile_options(client_demo PRIVATE -Wall -Wextra)
if(CLIENT_DEMO_WERROR)
  target_compile_options(client_demo PRIVATE -Werror)
endif()
install(TARGETS client_demo RUNTIME DESTINATION bin)
