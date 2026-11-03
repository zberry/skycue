# skycued: the daemon. skycued_core holds everything except main.cpp so the
# tests can link it.

# ASTERIX CAT021 codec and stream framer: pure code, no I/O.
add_library(skycued_asterix STATIC
  src/asterix/cat021.cpp
  src/asterix/cat021_encode.cpp
  src/asterix/cat021_framer.cpp)
target_include_directories(skycued_asterix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
skycue_set_warnings(skycued_asterix)

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
  src/file_util.cpp
  src/filters.cpp
  src/forwarder.cpp
  src/geo.cpp
  src/inputs/cat021_map.cpp
  src/inputs/device_time.cpp
  src/inputs/dump978_input.cpp
  src/inputs/input.cpp
  src/inputs/input_def.cpp
  src/inputs/input_manager.cpp
  src/inputs/inputs_file.cpp
  src/inputs/ping_cat021_input.cpp
  src/inputs/ping_json.cpp
  src/inputs/ping_json_input.cpp
  src/inputs/source_input.cpp
  src/inputs/tcp_connect_client.cpp
  src/inputs/tcp_listener.cpp
  src/inputs/transport.cpp
  src/inputs/udp_listener.cpp
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
  src/web_fisb.cpp
  src/web_server.cpp)
target_include_directories(skycued_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(skycued_core PUBLIC skycued_asterix skycue::skycue Boost::boost Boost::program_options)
skycue_set_warnings(skycued_core)

add_executable(skycued src/main.cpp)
target_link_libraries(skycued PRIVATE skycued_core Boost::program_options Threads::Threads)
skycue_set_warnings(skycued)
install(TARGETS skycued RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
