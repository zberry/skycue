# skycued: the daemon. skycued_core holds everything except main.cpp so the
# tests can link it.

# ASTERIX CAT021 codec, stream framer and the capture-file line: pure code,
# no I/O. skycue-dump links it without skycued_core, hence time_util here.
add_library(skycued_asterix STATIC
  src/asterix/capture_line.cpp
  src/asterix/cat021.cpp
  src/asterix/cat021_encode.cpp
  src/asterix/cat021_framer.cpp
  src/json_escape.cpp
  src/time_util.cpp
  src/track_names.cpp)
target_include_directories(skycued_asterix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(skycued_asterix PUBLIC skycue::skycue Boost::boost)
skycue_set_warnings(skycued_asterix)

# --convert-conf fills in the text of the example config: embed it.
set(_skycue_toml_example "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/skycue.toml.example")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_skycue_toml_example}")
file(READ "${_skycue_toml_example}" SKYCUE_TOML_EXAMPLE_TEXT)
configure_file(src/skycue_example_toml.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/skycue_example_toml.hpp" @ONLY)

add_library(skycued_core STATIC
  src/altitude_fit.cpp
  src/change_detector.cpp
  src/coding/bits.cpp
  src/coding/modes_crc.cpp
  src/decode/acas.cpp
  src/decode/altitude.cpp
  src/decode/commb.cpp
  src/decode/cpr.cpp
  src/decode/frame.cpp
  src/decode/uat_decoder.cpp
  src/decode/uat_frame.cpp
  src/decode/frame_source.cpp
  src/decode/modes_decoder.cpp
  src/check_inputs.cpp
  src/config.cpp
  src/conf_convert.cpp
  src/conf_legacy.cpp
  src/engine.cpp
  src/fisb/apdu.cpp
  src/fisb/dlac.cpp
  src/fisb/fisb_decoder.cpp
  src/fisb/fisb_json.cpp
  src/fisb/fisb_store.cpp
  src/fisb/global_block.cpp
  src/fisb/location_index.cpp
  src/fisb/segment_assembler.cpp
  src/fisb/text_products.cpp
  src/geom_altitude.cpp
  src/fisb/twgo.cpp
  src/fisb/uplink_frame.cpp
  src/file_util.cpp
  src/filters.cpp
  src/forwarder.cpp
  src/geo.cpp
  src/hub/hub.cpp
  src/hub/raw_tap.cpp
  src/icao_class.cpp
  src/inputs/beast_raw_input.cpp
  src/inputs/capture_writer.cpp
  src/inputs/cat021_map.cpp
  src/inputs/device_time.cpp
  src/inputs/input.cpp
  src/inputs/input_def.cpp
  src/inputs/input_manager.cpp
  src/inputs/inputs_file.cpp
  src/inputs/ping_cat021_input.cpp
  src/inputs/ping_encode.cpp
  src/inputs/ping_json.cpp
  src/inputs/ping_json_input.cpp
  src/inputs/readsb_stream_input.cpp
  src/inputs/source_input.cpp
  src/inputs/status_poller.cpp
  src/inputs/tcp_connect_client.cpp
  src/inputs/tcp_listener.cpp
  src/inputs/transport.cpp
  src/inputs/uat_raw_input.cpp
  src/inputs/udp_listener.cpp
  src/json_lite.cpp
  src/line_client.cpp
  src/log.cpp
  src/outputs/output_def.cpp
  src/raw_formats.cpp
  src/readsb_json_source.cpp
  src/recorder.cpp
  src/recording_codec.cpp
  src/recording_reader.cpp
  src/registration.cpp
  src/replay_source.cpp
  src/retention.cpp
  src/rotating_file.cpp
  src/runtime_config.cpp
  src/stats_report.cpp
  src/stats_store.cpp
  src/stats_store_json.cpp
  src/toml_lite.cpp
  src/observation_json.cpp
  src/track_table.cpp
  src/web_api.cpp
  src/web_config.cpp
  src/web_fisb.cpp
  src/web_inputs.cpp
  src/web_outputs.cpp
  src/web_reply.cpp
  src/web_stats.cpp
  src/web_server.cpp)
target_include_directories(skycued_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(skycued_core PUBLIC skycued_asterix skycue::skycue skycue::net Boost::boost Boost::program_options)
skycue_set_warnings(skycued_core)

add_executable(skycued src/main.cpp)
target_link_libraries(skycued PRIVATE skycued_core Boost::program_options Threads::Threads)
skycue_set_warnings(skycued)
install(TARGETS skycued RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
