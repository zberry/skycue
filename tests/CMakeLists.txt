add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE Boost::unit_test_framework)
target_compile_definitions(test_smoke PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_smoke)
add_test(NAME smoke COMMAND test_smoke)

add_executable(test_protocol test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE skycue::skycue Boost::unit_test_framework)
target_compile_definitions(test_protocol PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_protocol)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_client test_client.cpp)
target_link_libraries(test_client PRIVATE skycue::skycue Boost::unit_test_framework)
target_compile_definitions(test_client PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_client)
add_test(NAME client COMMAND test_client)
set_tests_properties(client PROPERTIES TIMEOUT 60)

get_filename_component(_skycue_example_conf
  "${CMAKE_CURRENT_SOURCE_DIR}/../../etc/skycue.conf.example" ABSOLUTE)
add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE skycued_core Boost::unit_test_framework)
target_compile_definitions(test_config PRIVATE BOOST_TEST_DYN_LINK
  SKYCUE_EXAMPLE_CONF="${_skycue_example_conf}")
skycue_set_warnings(test_config)
add_test(NAME config COMMAND test_config)

add_executable(test_readsb_json test_readsb_json.cpp)
target_link_libraries(test_readsb_json PRIVATE skycued_core Boost::unit_test_framework)
target_compile_definitions(test_readsb_json PRIVATE BOOST_TEST_DYN_LINK
  SKYCUE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
skycue_set_warnings(test_readsb_json)
add_test(NAME readsb_json COMMAND test_readsb_json)

add_executable(test_geo test_geo.cpp)
target_link_libraries(test_geo PRIVATE skycued_core Boost::unit_test_framework)
target_compile_definitions(test_geo PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_geo)
add_test(NAME geo COMMAND test_geo)

add_executable(test_registration test_registration.cpp)
target_link_libraries(test_registration PRIVATE skycued_core Boost::unit_test_framework)
target_compile_definitions(test_registration PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_registration)
add_test(NAME registration COMMAND test_registration)

add_executable(test_track_table test_track_table.cpp)
target_link_libraries(test_track_table PRIVATE skycued_core Boost::unit_test_framework)
target_compile_definitions(test_track_table PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_track_table)
add_test(NAME track_table COMMAND test_track_table)

add_executable(test_change_detector test_change_detector.cpp)
target_link_libraries(test_change_detector PRIVATE skycued_core Boost::unit_test_framework)
target_compile_definitions(test_change_detector PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_change_detector)
add_test(NAME change_detector COMMAND test_change_detector)

add_executable(test_filters test_filters.cpp)
target_link_libraries(test_filters PRIVATE skycued_core Boost::unit_test_framework)
target_compile_definitions(test_filters PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_filters)
add_test(NAME filters COMMAND test_filters)

add_executable(test_forwarder test_forwarder.cpp)
target_link_libraries(test_forwarder PRIVATE skycued_core Boost::unit_test_framework)
target_compile_definitions(test_forwarder PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_forwarder)
add_test(NAME forwarder COMMAND test_forwarder)
set_tests_properties(forwarder PROPERTIES TIMEOUT 60)

get_filename_component(_skycue_docs_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../docs" ABSOLUTE)
add_executable(test_recorder test_recorder.cpp)
target_link_libraries(test_recorder PRIVATE skycued_core Boost::unit_test_framework)
target_compile_definitions(test_recorder PRIVATE BOOST_TEST_DYN_LINK
  SKYCUE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  SKYCUE_DOCS_DIR="${_skycue_docs_dir}")
skycue_set_warnings(test_recorder)
add_test(NAME recorder COMMAND test_recorder)

add_executable(test_retention test_retention.cpp)
target_link_libraries(test_retention PRIVATE skycued_core Boost::unit_test_framework)
target_compile_definitions(test_retention PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_retention)
add_test(NAME retention COMMAND test_retention)
set_tests_properties(recorder PROPERTIES TIMEOUT 60)

# Builds a tiny consumer against libskycue through add_subdirectory(),
# through find_package() on an install of it, and linked into a shared
# library with SKYCUE_INSTALL=OFF.
add_test(NAME consumer_smoke
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/consumer_smoke.sh
    ${PROJECT_SOURCE_DIR}/libskycue
    ${CMAKE_CURRENT_SOURCE_DIR}/consumer-smoke
    ${CMAKE_CURRENT_BINARY_DIR}/consumer-smoke-work
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set(_smoke_boost_dir "")
if(Boost_DIR)
  set(_smoke_boost_dir "${Boost_DIR}")
endif()
string(REPLACE ";" "\\;" _smoke_prefix_path "${CMAKE_PREFIX_PATH}")
set_tests_properties(consumer_smoke PROPERTIES
  TIMEOUT 300
  ENVIRONMENT "CMAKE=${CMAKE_COMMAND};CXX=${CMAKE_CXX_COMPILER};SKYCUE_SMOKE_BOOST_DIR=${_smoke_boost_dir};SKYCUE_SMOKE_PREFIX_PATH=${_smoke_prefix_path}")

add_executable(test_raw_formats test_raw_formats.cpp)
target_link_libraries(test_raw_formats PRIVATE skycued_core Boost::unit_test_framework)
target_compile_definitions(test_raw_formats PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_raw_formats)
add_test(NAME raw_formats COMMAND test_raw_formats)

# skycue-rawrec / skycue-rawplay on the real-data fixtures; the last part
# feeds a real readsb when one has been built (the superbuild's stage).
get_filename_component(_skycue_stage_readsb
  "${PROJECT_SOURCE_DIR}/../build/stage/bin/readsb" ABSOLUTE)
set(SKYCUE_TEST_READSB "${_skycue_stage_readsb}" CACHE FILEPATH
  "readsb used by the rawtools full-stack check (skipped when missing)")
add_test(NAME rawtools
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_rawtools.sh
    $<TARGET_FILE:skycue-rawrec> $<TARGET_FILE:skycue-rawplay>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures ${SKYCUE_TEST_READSB})
set_tests_properties(rawtools PROPERTIES TIMEOUT 120)

add_executable(test_engine test_engine.cpp)
target_link_libraries(test_engine PRIVATE skycued_core Boost::unit_test_framework Threads::Threads)
target_compile_definitions(test_engine PRIVATE BOOST_TEST_DYN_LINK
  SKYCUE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
skycue_set_warnings(test_engine)
add_test(NAME engine COMMAND test_engine)
set_tests_properties(engine PROPERTIES TIMEOUT 120)

foreach(_t engine_api recording_independence runtime_config)
  add_executable(test_${_t} test_${_t}.cpp)
  target_link_libraries(test_${_t} PRIVATE skycued_core Boost::unit_test_framework Threads::Threads)
  target_compile_definitions(test_${_t} PRIVATE BOOST_TEST_DYN_LINK
    SKYCUE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  skycue_set_warnings(test_${_t})
  add_test(NAME ${_t} COMMAND test_${_t})
  set_tests_properties(${_t} PROPERTIES TIMEOUT 60)
endforeach()

# skycue-dump against a skycued replaying the readsb JSON fixture, TCP and UDP.
add_test(NAME dump
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_dump.sh
    $<TARGET_FILE:skycued> $<TARGET_FILE:skycue-dump> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
set_tests_properties(dump PROPERTIES TIMEOUT 60)

add_executable(test_stats test_stats.cpp)
target_link_libraries(test_stats PRIVATE skycued_core Boost::unit_test_framework)
target_compile_definitions(test_stats PRIVATE BOOST_TEST_DYN_LINK)
skycue_set_warnings(test_stats)
add_test(NAME stats COMMAND test_stats)

# skycue-stats on the readsb JSON fixture and on recordings of a skycued replaying it.
add_test(NAME stats_cli
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_stats_cli.sh
    $<TARGET_FILE:skycued> $<TARGET_FILE:skycue-stats> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
set_tests_properties(stats_cli PROPERTIES TIMEOUT 120)

add_executable(test_web_server test_web_server.cpp)
target_link_libraries(test_web_server PRIVATE skycued_core Boost::unit_test_framework Threads::Threads)
target_compile_definitions(test_web_server PRIVATE BOOST_TEST_DYN_LINK
  SKYCUE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  SKYCUED_EXE="$<TARGET_FILE:skycued>")
add_dependencies(test_web_server skycued)
skycue_set_warnings(test_web_server)
add_test(NAME web_server COMMAND test_web_server)
set_tests_properties(web_server PROPERTIES TIMEOUT 120)

# The web UI as skycued serves it from the staged www directory, plus the
# page's pure helpers under node when node is installed.
add_test(NAME web_ui
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_web_ui.sh
    $<TARGET_FILE:skycued> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures ${SKYCUE_WWW_STAGE}
    ${CMAKE_CURRENT_SOURCE_DIR}/js)
set_tests_properties(web_ui PROPERTIES TIMEOUT 60)

# Headless Chromium against a live skycued (dev machines only; see
# tests/ui/README.md). Says SKIP and passes without node_modules/@playwright/test.
if(SKYCUE_UI_TESTS)
  add_test(NAME ui_playwright
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/ui/run.sh
      $<TARGET_FILE:skycued> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures ${SKYCUE_WWW_STAGE})
  set_tests_properties(ui_playwright PROPERTIES TIMEOUT 300)
endif()

# FIS-B uplink frame, APDU header and DLAC parsers on hand-built vectors and
# the two uplink fixtures (zlib only for reading the .gz sample).
find_package(ZLIB REQUIRED)
add_executable(test_fisb_frame test_fisb_frame.cpp)
target_link_libraries(test_fisb_frame PRIVATE skycued_core Boost::unit_test_framework ZLIB::ZLIB)
target_compile_definitions(test_fisb_frame PRIVATE BOOST_TEST_DYN_LINK
  SKYCUE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
skycue_set_warnings(test_fisb_frame)
add_test(NAME fisb_frame COMMAND test_fisb_frame)

# FIS-B segment reassembly, TWGO records and product-413 text, on hand-built
# vectors and both uplink fixtures.
add_executable(test_fisb_twgo test_fisb_twgo.cpp)
target_link_libraries(test_fisb_twgo PRIVATE skycued_core Boost::unit_test_framework ZLIB::ZLIB)
target_compile_definitions(test_fisb_twgo PRIVATE BOOST_TEST_DYN_LINK
  SKYCUE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
skycue_set_warnings(test_fisb_twgo)
add_test(NAME fisb_twgo COMMAND test_fisb_twgo)

# FIS-B global-block rasters (63/64 NEXRAD, 70/71/84/90/91/103) on hand-built
# vectors and both uplink fixtures.
add_executable(test_fisb_blocks test_fisb_blocks.cpp)
target_link_libraries(test_fisb_blocks PRIVATE skycued_core Boost::unit_test_framework ZLIB::ZLIB)
target_compile_definitions(test_fisb_blocks PRIVATE BOOST_TEST_DYN_LINK
  SKYCUE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
skycue_set_warnings(test_fisb_blocks)
add_test(NAME fisb_blocks COMMAND test_fisb_blocks)
