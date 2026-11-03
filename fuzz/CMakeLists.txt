# libFuzzer harnesses (SKYCUE_FUZZ=ON, clang only; docs/hardening.md
# "Fuzzing"). Never part of a default build. Each harness calls a pure parser
# entry point, never a socket or a file; seeds are in seeds/<name>/.

skycue_add_fuzzer(readsb_line SOURCES fuzz_readsb_line.cpp)
skycue_add_fuzzer(raw_formats SOURCES fuzz_raw_formats.cpp)
skycue_add_fuzzer(recording SOURCES fuzz_recording.cpp)
skycue_add_fuzzer(fisb_decoder SOURCES fuzz_fisb_decoder.cpp)
skycue_add_fuzzer(iqgen_coding SOURCES fuzz_iqgen_coding.cpp LIBS skycue_iqgen_core)

# The harness names, one per line, for scripts/run-fuzz.sh --bin-dir.
get_property(_skycue_fuzz_names GLOBAL PROPERTY SKYCUE_FUZZ_TARGETS)
list(JOIN _skycue_fuzz_names "\n" _skycue_fuzz_list)
file(WRITE ${PROJECT_BINARY_DIR}/fuzz/targets.txt "${_skycue_fuzz_list}\n")
