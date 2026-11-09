skycue_add_unit_test(smoke LIBS)

skycue_add_unit_test(protocol LIBS skycue::skycue DEFS SKYCUE_CMAKE_VERSION="${PROJECT_VERSION}")

skycue_add_unit_test(client LIBS skycue::skycue NET_SUPPORT TIMEOUT 60)

get_filename_component(_skycue_example_conf
  "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/skycue.toml.example" ABSOLUTE)
skycue_add_unit_test(config DEFS SKYCUE_EXAMPLE_CONF="${_skycue_example_conf}" FIXTURE_DIR)

get_filename_component(_skycue_docs_dir "${CMAKE_CURRENT_SOURCE_DIR}/../docs" ABSOLUTE)
skycue_add_unit_test(config_keys DEFS SKYCUE_EXAMPLE_CONF="${_skycue_example_conf}"
  SKYCUE_DOCS_DIR="${_skycue_docs_dir}" FIXTURE_DIR)

skycue_add_unit_test(conf_convert)

skycue_add_unit_test(readsb_json FIXTURE_DIR)

skycue_add_unit_test(geo)

skycue_add_unit_test(stats_store)

# icao_class: the daemon's copy of the page's class/country rules, on the
# case fixture app_pure.test.js runs too.
skycue_add_unit_test(icao_class FIXTURE_DIR)

# icao_tables_current: skycued/src/icao_tables.inc is what
# scripts/fixtures/gen-icao-tables.py writes from www/app_pure.js. Skipped when the
# generator or the page is not there (skycue built inside a parent project)
# or python3 is not installed. icao_tables_generator (test_icao_generator.sh):
# the generator's exit status on broken copies of the page; skipped likewise.
get_filename_component(_skycue_gen_icao
  "${PROJECT_SOURCE_DIR}/scripts/fixtures/gen-icao-tables.py" ABSOLUTE)
set(_skycue_app_pure "${PROJECT_SOURCE_DIR}/www/app_pure.js")
set(_skycue_icao_inc "${PROJECT_SOURCE_DIR}/skycued/src/icao_tables.inc")
find_program(SKYCUE_PYTHON3 python3)
set(_skycue_icao_skip "")
if(NOT EXISTS "${_skycue_gen_icao}")
  set(_skycue_icao_skip "scripts/fixtures/gen-icao-tables.py not found")
elseif(NOT EXISTS "${_skycue_app_pure}")
  set(_skycue_icao_skip "www/app_pure.js not found")
elseif(NOT SKYCUE_PYTHON3)
  set(_skycue_icao_skip "python3 not found")
endif()
if(_skycue_icao_skip)
  skycue_skip_test(icao_tables_current "${_skycue_icao_skip}")
  skycue_skip_test(icao_tables_generator "${_skycue_icao_skip}")
else()
  add_test(NAME icao_tables_current
    COMMAND ${SKYCUE_PYTHON3} -I ${_skycue_gen_icao} --check ${_skycue_app_pure} ${_skycue_icao_inc})
  add_test(NAME icao_tables_generator
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_icao_generator.sh ${SKYCUE_PYTHON3}
      ${_skycue_gen_icao} ${_skycue_app_pure} ${_skycue_icao_inc} ${CMAKE_CURRENT_BINARY_DIR}/icao-generator)
  set_tests_properties(icao_tables_current icao_tables_generator PROPERTIES TIMEOUT 60)
endif()

skycue_add_unit_test(registration)

skycue_add_unit_test(track_table)

skycue_add_unit_test(change_detector)

skycue_add_unit_test(filters)

skycue_add_unit_test(forwarder NET_SUPPORT TIMEOUT 60)

skycue_add_unit_test(recorder DEFS SKYCUE_DOCS_DIR="${_skycue_docs_dir}" FIXTURE_DIR TIMEOUT 60)

skycue_add_unit_test(retention)

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

# examples/parent-project (a separate project using add_subdirectory on the
# whole source tree) built against a copy of it in a temp dir.
# Skipped (77) when the cmake it is given is not found.
add_test(NAME parent_project
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_parent_project.sh
    ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/parent-project-work
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(parent_project PROPERTIES
  TIMEOUT 300
  SKIP_RETURN_CODE 77
  ENVIRONMENT "CMAKE=${CMAKE_COMMAND};CXX=${CMAKE_CXX_COMPILER};SKYCUE_SMOKE_BOOST_DIR=${_smoke_boost_dir};SKYCUE_SMOKE_PREFIX_PATH=${_smoke_prefix_path}")

# install_layout: `cmake --install` of this build tree into a DESTDIR, checked
# against what this tree's configuration should produce (default source
# install, package build with SKYCUE_PACKAGE_CONFIG and absolute
# SKYCUE_SYSTEMD_UNITDIR, or SKYCUE_INSTALL_SYSTEM_FILES=OFF as a parent build).
add_test(NAME install_layout
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_install_layout.sh ${CMAKE_COMMAND}
    ${PROJECT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/install-layout-work
    ${CMAKE_INSTALL_PREFIX} ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_SYSCONFDIR}
    ${SKYCUE_INSTALL_SYSTEM_FILES} ${SKYCUE_PACKAGE_CONFIG} ${SKYCUE_SYSTEMD_UNITDIR})
set_tests_properties(install_layout PROPERTIES TIMEOUT 120)

# package_conf: cmake/PackageConfig.cmake on the example, and skycued on what
# it writes. It picks a closed port with python3, so it is skipped (exit 77)
# without one.
add_test(NAME package_conf
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_package_conf.sh ${CMAKE_COMMAND}
    ${PROJECT_SOURCE_DIR}/cmake/PackageConfig.cmake ${_skycue_example_conf}
    $<TARGET_FILE:skycued> ${CMAKE_CURRENT_BINARY_DIR}/package-conf-work "${SKYCUE_PYTHON3}")
set_tests_properties(package_conf PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# package_postinst: the upgrade step in debian/skycue.postinst and the spec
# (skycue.conf -> skycue.toml), run against a fake root with the built skycued.
add_test(NAME package_postinst
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_package_postinst.sh ${CMAKE_COMMAND}
    ${PROJECT_SOURCE_DIR}/cmake/PackageConfig.cmake ${_skycue_example_conf}
    $<TARGET_FILE:skycued> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/skycue_example_old.conf
    ${CMAKE_CURRENT_BINARY_DIR}/package-postinst-work)
set_tests_properties(package_postinst PROPERTIES TIMEOUT 60)

# SKYCUE_TEST_SLOW_FACTOR outside sanitizer and fuzz builds: two configures
# of this tree (no build), their registered timeouts and environments compared.
if(NOT SKYCUE_PYTHON3)
  skycue_skip_test(slow_factor "python3 not found")
else()
  add_test(NAME slow_factor
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_slow_factor.sh
      ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/slow-factor-work ${SKYCUE_PYTHON3}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(slow_factor PROPERTIES
    TIMEOUT 300
    ENVIRONMENT "CMAKE=${CMAKE_COMMAND};CTEST=${CMAKE_CTEST_COMMAND};CXX=${CMAKE_CXX_COMPILER};SKYCUE_SMOKE_BOOST_DIR=${_smoke_boost_dir};SKYCUE_SMOKE_PREFIX_PATH=${_smoke_prefix_path}")
endif()

skycue_add_unit_test(raw_formats)

# skycue-rawrec / skycue-rawplay on the real-data fixtures; the last part
# feeds a real readsb when SKYCUE_TEST_READSB names one.
set(SKYCUE_TEST_READSB "" CACHE FILEPATH
  "readsb used by the rawtools full-stack check (skipped when missing)")
add_test(NAME rawtools
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_rawtools.sh
    $<TARGET_FILE:skycue-rawrec> $<TARGET_FILE:skycue-rawplay>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures ${SKYCUE_TEST_READSB})
set_tests_properties(rawtools PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

# The output hub end to end: a replayed capture through a beast_raw input to
# TCP and multicast outputs, a stalled consumer cut, an output removed with a
# backlog. Skips without fixtures/raw (scripts/fixtures/fetch-test-captures.sh).
add_test(NAME hub
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_hub.sh
    $<TARGET_FILE:skycued> $<TARGET_FILE:skycue-rawplay> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
set_tests_properties(hub PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

skycue_add_unit_test(engine LIBS skycued_core Threads::Threads FIXTURE_DIR NET_SUPPORT TIMEOUT 120)

skycue_add_unit_test(stats_engine LIBS skycued_core Threads::Threads FIXTURE_DIR NET_SUPPORT
  TIMEOUT 120)

foreach(_t engine_api recording_independence runtime_config web_config check_inputs)
  skycue_add_unit_test(${_t} LIBS skycued_core Threads::Threads FIXTURE_DIR NET_SUPPORT TIMEOUT 60)
endforeach()

# test_check_inputs runs skycued --check-inputs for the inputs.decoder lines.
add_dependencies(test_check_inputs skycued)
target_compile_definitions(test_check_inputs PRIVATE SKYCUED_EXE="$<TARGET_FILE:skycued>")

# skycue-dump against a skycued replaying the readsb JSON fixture, TCP and UDP.
add_test(NAME dump
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_dump.sh
    $<TARGET_FILE:skycued> $<TARGET_FILE:skycue-dump> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
set_tests_properties(dump PROPERTIES TIMEOUT 60)

# skycue-pingsim (CAT021, JSON with status, capture replay, --tcp-listen) into
# one skycued, and skycue-dump --capture on the capture file.
add_test(NAME pingsim
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_pingsim.sh
    $<TARGET_FILE:skycued> $<TARGET_FILE:skycue-pingsim> $<TARGET_FILE:skycue-dump>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
set_tests_properties(pingsim PROPERTIES TIMEOUT 120)

skycue_add_unit_test(stats)

# skycue-stats on the readsb JSON fixture and on recordings of a skycued replaying it.
add_test(NAME stats_cli
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_stats_cli.sh
    $<TARGET_FILE:skycued> $<TARGET_FILE:skycue-stats> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
set_tests_properties(stats_cli PROPERTIES TIMEOUT 120)

# skycue-fisb on the FIS-B fixtures, including the tg30 golden output.
add_test(NAME fisb_cli
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_fisb_cli.sh
    $<TARGET_FILE:skycue-fisb> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
set_tests_properties(fisb_cli PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# Hand-written: SKYCUED_EXE needs a generator expression and a target dependency.
add_executable(test_web_server test_web_server.cpp)
target_link_libraries(test_web_server PRIVATE skycued_core Boost::unit_test_framework Threads::Threads)
target_compile_definitions(test_web_server PRIVATE BOOST_TEST_DYN_LINK
  SKYCUE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  SKYCUED_EXE="$<TARGET_FILE:skycued>"
  SKYCUE_CMAKE_VERSION="${PROJECT_VERSION}")
add_dependencies(test_web_server skycued)
skycue_set_warnings(test_web_server)
target_include_directories(test_web_server PRIVATE ${PROJECT_SOURCE_DIR}/net/tests) # net_test_support.hpp
add_test(NAME web_server COMMAND test_web_server)
set_tests_properties(web_server PROPERTIES TIMEOUT 120)

# /api/outputs, the cue clients' output field and the WebSocket outputs item
# over an Engine with a Hub and the cue Forwarder.
skycue_add_unit_test(web_outputs LIBS skycued_core Threads::Threads FIXTURE_DIR NET_SUPPORT
  TIMEOUT 120)

# The shared reply helpers and the exception-to-status ladders of the /api/* files.
skycue_add_unit_test(web_reply LIBS skycued_core Threads::Threads)

# The FIS-B routes and WebSocket items over a replay of the uplink fixture.
skycue_add_unit_test(fisb_web LIBS skycued_core Threads::Threads FIXTURE_DIR NET_SUPPORT
  TIMEOUT 120)

# The web UI as skycued serves it from the staged www directory, plus the
# page's pure helpers under node when node is installed.
add_test(NAME web_ui
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_web_ui.sh
    $<TARGET_FILE:skycued> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures ${SKYCUE_WWW_STAGE}
    ${CMAKE_CURRENT_SOURCE_DIR}/js)
set_tests_properties(web_ui PROPERTIES TIMEOUT 60)

# Headless Chromium against a live skycued (dev machines only; see
# tests/ui/README.md). Says SKIP and exits 77 (skipped) without
# node_modules/@playwright/test or a staged Leaflet.
if(SKYCUE_UI_TESTS)
  add_test(NAME ui_playwright
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/ui/run.sh
      $<TARGET_FILE:skycued> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures ${SKYCUE_WWW_STAGE}
      $<TARGET_FILE:skycue-pingsim>)
  set_tests_properties(ui_playwright PROPERTIES TIMEOUT 720 SKIP_RETURN_CODE 77)
endif()

# FIS-B uplink frame, APDU header and DLAC parsers on hand-built vectors and
# the two uplink fixtures (zlib only for reading the .gz sample).
find_package(ZLIB REQUIRED)
skycue_add_unit_test(fisb_frame LIBS skycued_core ZLIB::ZLIB FIXTURE_DIR)

# FIS-B segment reassembly, TWGO records and product-413 text, on hand-built
# vectors and both uplink fixtures.
skycue_add_unit_test(fisb_twgo LIBS skycued_core ZLIB::ZLIB FIXTURE_DIR)

# FIS-B global-block rasters (63/64 NEXRAD, 70/71/84/90/91/103) on hand-built
# vectors and both uplink fixtures.
skycue_add_unit_test(fisb_blocks LIBS skycued_core ZLIB::ZLIB FIXTURE_DIR)

# FisbStore on a fake clock (design §10).
skycue_add_unit_test(fisb_store)

# LocationIndex: learned report positions, persistence, operator CSV (design §3.2).
skycue_add_unit_test(location_index)

# FisbDecoder end to end, Engine/Recorder/replay integration.
skycue_add_unit_test(fisb_decoder LIBS skycued_core ZLIB::ZLIB Threads::Threads FIXTURE_DIR
  NET_SUPPORT TIMEOUT 120)

# Mode S CRC, UAT Reed-Solomon and framing for the IQ generators, on
# hand-built vectors and the raw UAT fixture.
skycue_add_unit_test(iqgen LIBS skycue_iqgen_core FIXTURE_DIR)
add_test(NAME iqgen_cli
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_iqgen_cli.sh
    $<TARGET_FILE:skycue-iqgen> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
set_tests_properties(iqgen_cli PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

# ASTERIX CAT021 decoder, test encoder and TCP stream framer (pure).
skycue_add_unit_test(cat021 LIBS skycued_asterix)

# pingStation 3 adapters, pure part: device time, JSON parser, CAT021 mapping.
skycue_add_unit_test(pingstation FIXTURE_DIR)

# Input definitions, their validation, the inputs file and the settled-file watcher.
skycue_add_unit_test(input_def)

# Output definitions (the hub's destinations) and the raw-message tap.
skycue_add_unit_test(output_def)

skycue_add_unit_test(raw_tap)

# The hub core: per-kind buses and output lifecycle.
skycue_add_unit_test(hub_core LIBS skycued_core Threads::Threads NET_SUPPORT TIMEOUT 60)

# pingStation transports: UDP listener, TCP connect client, TCP listener.
skycue_add_unit_test(transports LIBS skycued_core Threads::Threads FIXTURE_DIR NET_SUPPORT
  TIMEOUT 60)

# Mode S CRC, bit helpers and single-bit syndrome correction.
skycue_add_unit_test(coding)

# Comm-B BDS 4,0/5,0/6,0 inference, ACAS RA decoding (frame level, pure).
skycue_add_unit_test(decode_commb)

# Mode S frame fields and altitude/squawk code tables.
skycue_add_unit_test(decode_fields)

# CPR position math: global, surface and local decoding with NL.
skycue_add_unit_test(decode_cpr FIXTURE_DIR)

# The 1090 decoder: address confirmation, CPR pairing, observation production.
skycue_add_unit_test(decode_modes)

# Damaged input: random bytes, bit flips, key table limits, wrong frame lengths.
skycue_add_unit_test(decode_robustness TIMEOUT 120)

# Inputs and the InputManager: pingStation inputs over real sockets, the
# inputs file, hand edits and the lifecycle.
skycue_add_unit_test(inputs LIBS skycued_core Threads::Threads FIXTURE_DIR NET_SUPPORT TIMEOUT 120)

# Every input's raw messages on the tap; readsb and pingStation JSON on any transport; uat_raw.
skycue_add_unit_test(raw_inputs LIBS skycued_core Threads::Threads NET_SUPPORT TIMEOUT 120)

# The pingStation status poll over HTTP against an in-test server.
skycue_add_unit_test(status_poller LIBS skycued_core Threads::Threads FIXTURE_DIR NET_SUPPORT
  TIMEOUT 60)

# pingStation raw capture (capture: true) and the RECORD_RAW frames of JSON
# mode, in the formats skycue-rawplay replays.
skycue_add_unit_test(capture LIBS skycued_core Threads::Threads FIXTURE_DIR NET_SUPPORT TIMEOUT 60)

# The beast_raw input (Beast and AVR over any transport), its re-encode and the
# raw captures of beast_raw and uat_raw.
skycue_add_unit_test(beast_raw LIBS skycued_core Threads::Threads FIXTURE_DIR NET_SUPPORT
  TIMEOUT 60)

# The sanitizer canary (scripts/dev/run-sanitizers.sh --canary); never built by
# default.
# Hand-written: EXCLUDE_FROM_ALL, no ctest entry.
add_executable(sanitizer_canary EXCLUDE_FROM_ALL sanitizer/canary.cpp)
target_link_libraries(sanitizer_canary PRIVATE Threads::Threads)

# fuzz_smoke: every libFuzzer harness for 5 s on its seeds, through
# scripts/dev/run-fuzz.sh. fuzz_build (fixture setup) builds the harnesses first,
# since ctest has no target dependencies.
get_filename_component(_skycue_run_fuzz
  "${PROJECT_SOURCE_DIR}/scripts/dev/run-fuzz.sh" ABSOLUTE)
if(NOT SKYCUE_FUZZ)
  skycue_skip_test(fuzz_smoke "SKYCUE_FUZZ is OFF (fuzz harnesses build with clang only)")
elseif(NOT EXISTS "${_skycue_run_fuzz}")
  skycue_skip_test(fuzz_smoke "scripts/dev/run-fuzz.sh not found")
else()
  get_property(_skycue_fuzz_names GLOBAL PROPERTY SKYCUE_FUZZ_TARGETS)
  set(_skycue_fuzz_bins "")
  foreach(_n IN LISTS _skycue_fuzz_names)
    list(APPEND _skycue_fuzz_bins fuzz_${_n})
  endforeach()
  add_test(NAME fuzz_build
    COMMAND ${CMAKE_COMMAND} --build ${PROJECT_BINARY_DIR} --target ${_skycue_fuzz_bins})
  set_tests_properties(fuzz_build PROPERTIES FIXTURES_SETUP fuzz_bins)
  add_test(NAME fuzz_smoke
    COMMAND bash ${_skycue_run_fuzz} all --seconds 5
      --bin-dir ${PROJECT_BINARY_DIR}/fuzz
      --work ${CMAKE_CURRENT_BINARY_DIR}/fuzz-smoke
      --findings ${CMAKE_CURRENT_BINARY_DIR}/fuzz-smoke/findings)
  set_tests_properties(fuzz_smoke PROPERTIES
    FIXTURES_REQUIRED fuzz_bins TIMEOUT 600 SKIP_RETURN_CODE 77)
endif()

# static_analysis: clang-tidy over skycue's own sources, through
# scripts/dev/run-static.sh --tidy-only (it configures its own tree under this
# build directory); fails on any unsuppressed warning.
get_filename_component(_skycue_run_static
  "${PROJECT_SOURCE_DIR}/scripts/dev/run-static.sh" ABSOLUTE)
find_program(SKYCUE_CLANG_TIDY clang-tidy)
if(NOT SKYCUE_STATIC_TESTS)
  skycue_skip_test(static_analysis "SKYCUE_STATIC_TESTS is OFF")
elseif(NOT SKYCUE_CLANG_TIDY)
  skycue_skip_test(static_analysis "clang-tidy not found")
elseif(NOT EXISTS "${_skycue_run_static}")
  skycue_skip_test(static_analysis "scripts/dev/run-static.sh not found")
else()
  add_test(NAME static_analysis
    COMMAND bash ${_skycue_run_static} --tidy-only
      --build-dir ${CMAKE_CURRENT_BINARY_DIR}/static-analysis)
  set_tests_properties(static_analysis PROPERTIES TIMEOUT 1800 SKIP_RETURN_CODE 77)
endif()

# soak_eval: the soak pass rule (tests/soak/soak_eval.py) on synthetic series.
add_test(NAME soak_eval
  COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/soak/soak_eval.py --self-test)
set_tests_properties(soak_eval PROPERTIES TIMEOUT 60)

# soak_short: tests/soak/soak.sh for 2 minutes. It proves the CSV is written
# and the rule evaluates; a 2-minute RSS series says nothing about leaks, so
# its growth limit is 1.0 (the 4-hour run uses the default, docs/hardening.md).
if(NOT SKYCUE_SOAK_TESTS)
  skycue_skip_test(soak_short "SKYCUE_SOAK_TESTS is OFF")
else()
  add_test(NAME soak_short
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/soak/soak.sh
      --skycued $<TARGET_FILE:skycued>
      --pingsim $<TARGET_FILE:skycue-pingsim>
      --dump $<TARGET_FILE:skycue-dump>
      --www ${SKYCUE_WWW_STAGE}
      --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
      --minutes 2 --interval 5 --growth 1.0
      --out ${CMAKE_CURRENT_BINARY_DIR}/soak-short)
  set_tests_properties(soak_short PROPERTIES TIMEOUT 400 SKIP_RETURN_CODE 77)
endif()

# Script tests: every tests/scripts/test_*.sh runs as ctest "script_<name>" (exit
# 77 is a skip). The twelve today:
#   script_test_capture_seeds    script_test_docs_libskycue_example   script_test_fetch_test_captures
#   script_test_fetch_web_libs   script_test_make_locations   script_test_make_maps   script_test_make_tiles
#   script_test_net_standalone   script_test_package_metadata   script_test_package_scripts
#   script_test_skycue_web_tarballs   script_test_source_tarball
file(GLOB _script_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_*.sh")
list(FILTER _script_tests EXCLUDE REGEX "/test_package_postinst\\.sh$")  # registered above (takes arguments)
foreach(f IN LISTS _script_tests)
  get_filename_component(name "${f}" NAME_WE)
  add_test(NAME script_${name} COMMAND bash ${f})
  set_tests_properties(script_${name} PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)
endforeach()

# script_test_net_standalone (registered by the glob above) configures and
# builds a copy of net/ alone: the cmake and compiler of this build, a
# timeout of 300 s.
if(TEST script_test_net_standalone)
  set_tests_properties(script_test_net_standalone PROPERTIES
    TIMEOUT 300
    ENVIRONMENT "CMAKE=${CMAKE_COMMAND};CXX=${CMAKE_CXX_COMPILER};SKYCUE_SMOKE_BOOST_DIR=${_smoke_boost_dir};SKYCUE_SMOKE_PREFIX_PATH=${_smoke_prefix_path}")
endif()

# Beast and AVR frames as decoder input.
skycue_add_unit_test(decode_frame_source)

# The 1090 decoder against readsb on the 120 s capture: positions, altitude,
# speed, rates and identity from fixtures/raw/modes-120s.golden.jsonl.gz
# (skips when the fetched capture or the golden is absent).
find_package(ZLIB REQUIRED)
skycue_add_unit_test(decode_golden LIBS skycued_core ZLIB::ZLIB FIXTURE_DIR TIMEOUT 120)

# The 1090 decoder against OpenSky's decoded fields for the same frames
# (fixtures/opensky/*, fetched and unpacked by fetch-test-captures.sh; skips
# when absent).
skycue_add_unit_test(decode_opensky FIXTURE_DIR TIMEOUT 120)

# ACAS resolution advisories and rollcall altitude / identity against the same OpenSky
# tables (fixtures/opensky/*_acas_data4.csv, *_rollcall_replies_data4.csv; skips when absent).
skycue_add_unit_test(decode_acas_opensky FIXTURE_DIR TIMEOUT 120 SKIPPABLE)

# AltitudeFit: rolling GNSS-minus-baro fit over all aircraft.
skycue_add_unit_test(altitude_fit)

# Per-track geometric altitude policy: reported or estimated.
skycue_add_unit_test(geom_altitude)

# Geometric altitude evaluation on the data: leave-one-aircraft-out fit on the
# 120 s capture and own-difference hold on the OpenSky fixtures (skips when absent).
skycue_add_unit_test(geom_eval LIBS skycued_core ZLIB::ZLIB FIXTURE_DIR TIMEOUT 120 SKIPPABLE)

# skycued's own decoder against readsb, live: the 120 s capture replayed at speed 1
# through a beast_raw input under inputs.decoder = "skycued", compared with the committed
# golden and, with SKYCUE_READSB set, a fresh readsb run. Skips without fixtures/raw.
add_test(NAME decode_differential
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_decode_differential.sh
    $<TARGET_FILE:skycued> $<TARGET_FILE:skycue-rawplay> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
set_tests_properties(decode_differential PROPERTIES TIMEOUT 400 SKIP_RETURN_CODE 77)

# UAT (978 MHz) frame layout, line parser and pure field decoders.
skycue_add_unit_test(decode_uat_frame)

# UatDecoder: observations, range check, geometric altitude.
skycue_add_unit_test(decode_uat)

# The UAT frame decoders against dump978-fa, field by field, on the sample's 439
# downlinks (fixtures/raw/uat-sample.golden.jsonl.gz) and the two Stratux captures' 3760
# and 303 (uat-stratux.golden.jsonl.gz, uat-stratux-b.golden.jsonl.gz); skips when a
# golden is absent.
skycue_add_unit_test(decode_uat_golden LIBS skycued_core ZLIB::ZLIB FIXTURE_DIR TIMEOUT 120
  SKIPPABLE)

# Synthetic UAT coverage matrix (about 110 frames built with encodeUat): uat_synth prints the
# frames and the values built into them (fixtures/gen_uat_synthetic.sh runs it, then dump978-fa);
# the test compares dump978-fa's decode of them, the built-in values and our decoder; skips when a
# golden is absent.
add_executable(uat_synth uat_synth.cpp)
skycue_set_warnings(uat_synth)
skycue_add_unit_test(decode_uat_synthetic LIBS skycued_core ZLIB::ZLIB FIXTURE_DIR TIMEOUT 120
  SKIPPABLE)

# pingStation `source: decode`: rawData frames of both bands through the decoders, the 1 s duplicate
# rule, the reference from the status position, and device mode unchanged.
skycue_add_unit_test(ping_decode LIBS skycued_core Threads::Threads FIXTURE_DIR NET_SUPPORT
  TIMEOUT 120)

# UatRawInput with the 978 decoder on (inputs.decoder = "skycued"): the dump978 sample and the
# Stratux log through a real input, the counters and the double-feed warning.
skycue_add_unit_test(uat_raw_decode LIBS skycued_core Threads::Threads ZLIB::ZLIB FIXTURE_DIR
  NET_SUPPORT TIMEOUT 120)

# skycued's own UAT decoder against dump978-fa, live: the 439-downlink sample replayed at speed 1
# (about 2 minutes), then the Stratux captures, through a uat_raw input under inputs.decoder = "skycued", compared per address with
# the committed golden and, with SKYCUE_DUMP978 set, a fresh dump978-fa run. Skips without fixtures.
add_test(NAME decode_differential_uat
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_decode_differential_uat.sh
    $<TARGET_FILE:skycued> $<TARGET_FILE:skycue-rawplay> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
    $<TARGET_FILE:skycue-iqgen>)
set_tests_properties(decode_differential_uat PROPERTIES TIMEOUT 400 SKIP_RETURN_CODE 77)

skycue_add_unit_test(toml_lite DEFS SKYCUE_EXAMPLE_CONF="${_skycue_example_conf}" TIMEOUT 60)

skycue_add_unit_test(log TIMEOUT 60)

# skycued's configuration command line: --dump-env, --set, --convert-conf, the legacy flags.
add_test(NAME config_cli
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_config_cli.sh
    $<TARGET_FILE:skycued> ${_skycue_example_conf}
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/skycue_example_old.conf ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
set_tests_properties(config_cli PROPERTIES TIMEOUT 60)

skycue_add_unit_test(ecef LIBS skycue::skycue TIMEOUT 60)

# Timeouts and SKYCUE_TEST_SLOW scaled by the slow factor (Hardening.cmake).
skycue_scale_test_timeouts()
