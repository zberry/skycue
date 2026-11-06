# libFuzzer harnesses (SKYCUE_FUZZ=ON, clang only; docs/hardening.md
# "Fuzzing"). Never part of a default build. Each harness calls a pure parser
# entry point, never a socket, except fuzz_ws (below); a harness that needs
# files makes them under $SKYCUE_FUZZ_WORK/<name>/. Seeds are in seeds/<name>/; the ones cut from
# third-party captures are not committed: scripts/fixtures/gen-capture-seeds.py generates
# them at fuzz time (scripts/dev/run-fuzz.sh).

skycue_add_fuzzer(readsb_line SOURCES fuzz_readsb_line.cpp)
skycue_add_fuzzer(raw_formats SOURCES fuzz_raw_formats.cpp)
skycue_add_fuzzer(recording SOURCES fuzz_recording.cpp)
skycue_add_fuzzer(fisb_decoder SOURCES fuzz_fisb_decoder.cpp)
skycue_add_fuzzer(iqgen_coding SOURCES fuzz_iqgen_coding.cpp LIBS skycue_iqgen_core)
skycue_add_fuzzer(web SOURCES fuzz_web.cpp)
# The one harness with a socket: a real WebServer on an ephemeral loopback
# port in its own process (fuzz_ws.cpp).
skycue_add_fuzzer(ws SOURCES fuzz_ws.cpp LIBS Threads::Threads)
skycue_add_fuzzer(pingstation SOURCES fuzz_pingstation.cpp)
skycue_add_fuzzer(config_files SOURCES fuzz_config_files.cpp)

# The harness names, one per line, for scripts/dev/run-fuzz.sh --bin-dir.
get_property(_skycue_fuzz_names GLOBAL PROPERTY SKYCUE_FUZZ_TARGETS)
list(JOIN _skycue_fuzz_names "\n" _skycue_fuzz_list)
file(WRITE ${PROJECT_BINARY_DIR}/fuzz/targets.txt "${_skycue_fuzz_list}\n")
