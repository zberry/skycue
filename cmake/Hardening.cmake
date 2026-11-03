include_guard(GLOBAL)

# Instrumented builds for the hardening runs (docs/hardening.md). None of
# this is on by default: the default configure, the superbuild and the
# offline bundle build exactly what they built before.

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

set(SKYCUE_TEST_SLOW_FACTOR "" CACHE STRING
  "Multiplier for test timeouts and waits (SKYCUE_TEST_SLOW); empty = 4 in a sanitizer or fuzz build, else 1")
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

# skycue_skip_test(<name> <reason>): a test that prints "SKIP: <reason>"
# and exits 77, which ctest reports as Skipped.
function(skycue_skip_test name reason)
  add_test(NAME ${name} COMMAND sh -c "echo \"SKIP: $1\"; exit 77" sh "${reason}")
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()
