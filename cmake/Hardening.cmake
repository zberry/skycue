include_guard(GLOBAL)

# Instrumented builds for the hardening runs (docs/hardening.md). None of
# this is on by default: the default configure builds exactly what it built before.

set(SKYCUE_SANITIZE "OFF" CACHE STRING
  "Build skycue instrumented: address,undefined, thread or OFF (run with scripts/run-sanitizers.sh)")
set_property(CACHE SKYCUE_SANITIZE PROPERTY STRINGS "OFF" "address,undefined" "thread")
if(SKYCUE_SANITIZE STREQUAL "OFF" OR SKYCUE_SANITIZE STREQUAL "")
  set(_skycue_sanitize "")
elseif(SKYCUE_SANITIZE STREQUAL "address,undefined" OR SKYCUE_SANITIZE STREQUAL "thread")
  set(_skycue_sanitize "${SKYCUE_SANITIZE}")
else()
  message(FATAL_ERROR "SKYCUE_SANITIZE must be address,undefined, thread or OFF")
endif()

option(SKYCUE_FUZZ
  "Build the libFuzzer harnesses in fuzz (clang only; run with scripts/run-fuzz.sh)" OFF)
if(SKYCUE_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SKYCUE_FUZZ=ON needs clang (CMAKE_CXX_COMPILER_ID is ${CMAKE_CXX_COMPILER_ID})")
  endif()
  if(_skycue_sanitize STREQUAL "thread")
    message(FATAL_ERROR "SKYCUE_FUZZ=ON cannot be combined with SKYCUE_SANITIZE=thread")
  endif()
endif()

option(SKYCUE_STATIC_TESTS
  "Register the ctest static_analysis (scripts/run-static.sh --tidy-only; needs clang-tidy)" OFF)

option(SKYCUE_SOAK_TESTS
  "Register the ctest soak_short (tests/soak/soak.sh for 2 minutes; needs node and curl)" OFF)

set(SKYCUE_TEST_SLOW_FACTOR "" CACHE STRING
  "Multiplier for test timeouts and waits (SKYCUE_TEST_SLOW), in any build; empty = 4 in a sanitizer or fuzz build, else unscaled")
if(NOT SKYCUE_TEST_SLOW_FACTOR STREQUAL "")
  if(NOT SKYCUE_TEST_SLOW_FACTOR MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "SKYCUE_TEST_SLOW_FACTOR must be a whole number of at least 1")
  endif()
  set(_skycue_slow_factor "${SKYCUE_TEST_SLOW_FACTOR}")
elseif(_skycue_sanitize OR SKYCUE_FUZZ)
  set(_skycue_slow_factor 4)
else()
  set(_skycue_slow_factor 1)
endif()

# Where scripts/run-sanitizers.sh points the sanitizers' log_path; cached so
# the script reads it from CMakeCache.txt.
set(SKYCUE_SANITIZER_LOG_DIR "${PROJECT_BINARY_DIR}/sanitizer-logs"
  CACHE INTERNAL "sanitizer log_path directory (scripts/run-sanitizers.sh)")

if(_skycue_sanitize)
  # Line tables only: enough for symbolized stacks, a fraction of the disk
  # of full debug info.
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_skycue_san_debug -gline-tables-only)
  else()
    set(_skycue_san_debug -g1)
  endif()
  add_compile_options(-fsanitize=${_skycue_sanitize} -fno-omit-frame-pointer
    -fno-sanitize-recover=all ${_skycue_san_debug})
  add_link_options(-fsanitize=${_skycue_sanitize})
  message(STATUS "skycue: instrumented with -fsanitize=${_skycue_sanitize}, test slow factor ${_skycue_slow_factor}")
endif()

if(SKYCUE_FUZZ)
  # Everything gets the coverage hooks (fuzzer-no-link) and ASan + UBSan, so
  # the parsers a harness calls are instrumented; only the fuzz_* executables
  # link the libFuzzer driver (skycue_add_fuzzer). The other executables and
  # the tests still build and run in this tree.
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer -fno-sanitize-recover=all -gline-tables-only)
  add_link_options(-fsanitize=address,undefined)
  message(STATUS "skycue: fuzz build (-fsanitize=fuzzer-no-link,address,undefined), test slow factor ${_skycue_slow_factor}")
endif()

# skycue_add_fuzzer(<name> SOURCES <files...> [LIBS <targets...>]): the
# libFuzzer executable fuzz_<name> in <build>/fuzz, linked with skycued_core
# and LIBS; <name> is appended to the global property SKYCUE_FUZZ_TARGETS
# (read by tests/CMakeLists.txt for fuzz_smoke).
function(skycue_add_fuzzer name)
  cmake_parse_arguments(PARSE_ARGV 1 _fz "" "" "SOURCES;LIBS")
  if(NOT _fz_SOURCES)
    message(FATAL_ERROR "skycue_add_fuzzer(${name}): SOURCES is required")
  endif()
  add_executable(fuzz_${name} ${_fz_SOURCES})
  target_include_directories(fuzz_${name} PRIVATE ${PROJECT_SOURCE_DIR}/fuzz)
  target_link_libraries(fuzz_${name} PRIVATE skycued_core ${_fz_LIBS})
  target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
  set_target_properties(fuzz_${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/fuzz)
  skycue_set_warnings(fuzz_${name})
  set_property(GLOBAL APPEND PROPERTY SKYCUE_FUZZ_TARGETS ${name})
endfunction()

# skycue_scale_test_timeouts(): called at the end of each directory that
# registers tests. In instrumented builds (SKYCUE_SANITIZE or SKYCUE_FUZZ),
# and in any build given SKYCUE_TEST_SLOW_FACTOR (a slow or emulated build
# machine), every test of the calling directory gets its timeout (1500 s when
# it sets none) and its waits (SKYCUE_TEST_SLOW, tests/test_slow.hpp) scaled
# by the slow factor. Otherwise tests keep their own timeouts. The sanitizer
# options and suppression files are not set here: scripts/run-sanitizers.sh
# exports them at test time and ctest passes them on.
function(skycue_scale_test_timeouts)
  if(NOT (_skycue_sanitize OR SKYCUE_FUZZ OR NOT SKYCUE_TEST_SLOW_FACTOR STREQUAL ""))
    return()
  endif()
  get_property(_tests DIRECTORY PROPERTY TESTS)
  foreach(_t IN LISTS _tests)
    get_test_property(${_t} TIMEOUT _timeout)
    if(NOT _timeout)
      set(_timeout 1500)
    endif()
    math(EXPR _timeout "${_timeout} * ${_skycue_slow_factor}")
    set_tests_properties(${_t} PROPERTIES TIMEOUT ${_timeout})
    set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT "SKYCUE_TEST_SLOW=${_skycue_slow_factor}")
  endforeach()
endfunction()

# skycue_skip_test(<name> <reason>): a test that prints "SKIP: <reason>"
# and exits 77, which ctest reports as Skipped.
function(skycue_skip_test name reason)
  add_test(NAME ${name} COMMAND sh -c "echo \"SKIP: $1\"; exit 77" sh "${reason}")
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()
