# libskycue: track types, the wire-protocol codec and the client.
# Usable on its own through add_subdirectory(): it finds its own dependencies
# when the enclosing tree has not.

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

target_include_directories(skycue PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(skycue PUBLIC cxx_std_17)
target_link_libraries(skycue PUBLIC Boost::system Threads::Threads)

if(COMMAND skycue_set_warnings)
  skycue_set_warnings(skycue)
endif()
