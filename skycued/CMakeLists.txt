# skycued: the daemon. skycued_core holds everything except main.cpp so the
# tests can link it.
add_library(skycued_core STATIC
  src/change_detector.cpp
  src/conf_rewrite.cpp
  src/config.cpp
  src/dump978_raw_source.cpp
  src/engine.cpp
  src/fisb/apdu.cpp
  src/fisb/dlac.cpp
  src/fisb/fisb_decoder.cpp
  src/fisb/fisb_json.cpp
  src/fisb/fisb_store.cpp
  src/fisb/global_block.cpp
  src/fisb/segment_assembler.cpp
  src/fisb/text_products.cpp
  src/fisb/twgo.cpp
  src/fisb/uplink_frame.cpp
  src/filters.cpp
  src/forwarder.cpp
  src/geo.cpp
  src/json_lite.cpp
  src/line_client.cpp
  src/log.cpp
  src/raw_formats.cpp
  src/readsb_json_source.cpp
  src/recorder.cpp
  src/recording_reader.cpp
  src/registration.cpp
  src/replay_source.cpp
  src/retention.cpp
  src/rotating_file.cpp
  src/runtime_config.cpp
  src/stats_report.cpp
  src/time_util.cpp
  src/track_table.cpp
  src/web_api.cpp
  src/web_server.cpp)
target_include_directories(skycued_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(skycued_core PUBLIC skycue::skycue Boost::boost Boost::program_options)
skycue_set_warnings(skycued_core)

add_executable(skycued src/main.cpp)
target_link_libraries(skycued PRIVATE skycued_core Boost::program_options Threads::Threads)
skycue_set_warnings(skycued)
install(TARGETS skycued RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
