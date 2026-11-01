# skycued: the daemon. skycued_core holds everything except main.cpp so the
# tests can link it.
add_library(skycued_core STATIC
  src/config.cpp
  src/log.cpp)
target_include_directories(skycued_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(skycued_core PUBLIC skycue::skycue Boost::program_options)
skycue_set_warnings(skycued_core)
