# skycued: the daemon. skycued_core holds everything except main.cpp so the
# tests can link it.
add_library(skycued_core STATIC
  src/change_detector.cpp
  src/config.cpp
  src/filters.cpp
  src/geo.cpp
  src/json_lite.cpp
  src/log.cpp
  src/readsb_json_source.cpp
  src/registration.cpp
  src/track_table.cpp)
target_include_directories(skycued_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(skycued_core PUBLIC skycue::skycue Boost::boost Boost::program_options)
skycue_set_warnings(skycued_core)
