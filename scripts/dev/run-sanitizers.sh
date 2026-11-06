#!/usr/bin/env bash
# Builds skycue instrumented and runs its ctest suite under the sanitizers.
#
#   scripts/dev/run-sanitizers.sh MODE [--build-dir DIR] [-j N] [--canary] [--clean] [-- CTEST_ARGS...]
#   scripts/dev/run-sanitizers.sh --print-env MODE [--build-dir DIR]
#
# MODE is asan (ASan + UBSan, -DSKYCUE_SANITIZE=address,undefined, default
# tree <repo>/build-asan) or tsan (-DSKYCUE_SANITIZE=thread, <repo>/build-tsan).
# The compiler is ${CXX:-clang++}; asan falls back to g++ without clang++,
# tsan skips (gcc's TSan does not start on every kernel).
#
# Every sanitizer writes its reports to the tree's SKYCUE_SANITIZER_LOG_DIR
# (read from CMakeCache.txt; <build>/sanitizer-logs/), including
# reports from child processes (a skycued spawned by a shell test), which
# would otherwise only reach a test's output. A run fails when any test fails
# or any log file appears. The environment is exported here at test time, so
# a suppression file added later is used without reconfiguring.
#
#   --canary     build and run sanitizer_canary instead of the suite: passes
#                (exit 0, "canary: detected") only when each child's report
#                reached the log directory (an ASan and a UBSan report for
#                asan, a TSan report for tsan).
#   --clean      remove the build tree after the run; refused for a directory
#                that exists without a CMakeCache.txt.
#   --print-env  print the export lines for running an instrumented ctest by hand.
#
# Exit status: 0 clean, 1 a failed test or any sanitizer report, 77 skipped.
set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)

usage() {
  sed -n '4,5p' "${BASH_SOURCE[0]}" | sed 's/^# *//' >&2
  exit 2
}

print_env=0
mode=""
build_dir=""
jobs=""
canary=0
clean=0
ctest_args=()

while [ $# -gt 0 ]; do
  case "$1" in
    --print-env) print_env=1; shift; [ $# -gt 0 ] || usage; mode=$1 ;;
    --build-dir) shift; [ $# -gt 0 ] || usage; build_dir=$1 ;;
    -j) shift; [ $# -gt 0 ] || usage; jobs=$1 ;;
    -j*) jobs=${1#-j} ;;
    --canary) canary=1 ;;
    --clean) clean=1 ;;
    --) shift; ctest_args=("$@"); break ;;
    -h|--help) usage ;;
    asan|tsan) [ -z "$mode" ] || usage; mode=$1 ;;
    *) echo "run-sanitizers: unknown argument: $1" >&2; usage ;;
  esac
  shift
done

case "$mode" in
  asan) sanitize="address,undefined" ;;
  tsan) sanitize="thread" ;;
  *) usage ;;
esac
[ -n "$build_dir" ] || build_dir="$repo/build-$mode"
case "$build_dir" in
  /*) ;;
  *) build_dir="$PWD/$build_dir" ;;
esac
case "$jobs" in
  "") jobs=$(nproc 2>/dev/null || echo 2) ;;
  *[!0-9]*|0) echo "run-sanitizers: -j needs a positive number" >&2; exit 2 ;;
esac
if [ "$clean" = 1 ] && [ -e "$build_dir" ] && [ ! -f "$build_dir/CMakeCache.txt" ]; then
  echo "run-sanitizers: --clean refused: $build_dir has no CMakeCache.txt" >&2
  exit 2
fi

# The log directory: SKYCUE_SANITIZER_LOG_DIR from the tree's cache, or the
# value Hardening.cmake gives it when the tree is not configured yet.
read_log_dir() {
  log_dir=""
  if [ -f "$build_dir/CMakeCache.txt" ]; then
    log_dir=$(sed -n 's/^SKYCUE_SANITIZER_LOG_DIR:INTERNAL=//p' "$build_dir/CMakeCache.txt")
  fi
  [ -n "$log_dir" ] || log_dir="$build_dir/sanitizer-logs"
}
read_log_dir

# The environment every instrumented process runs with (H1, H2).
env_lines() {
  local tsan="halt_on_error=1:second_deadlock_stack=1:log_path=$log_dir/tsan"
  if [ -f "$repo/tests/sanitizer/tsan.supp" ]; then
    tsan="$tsan:suppressions=$repo/tests/sanitizer/tsan.supp"
  fi
  # Only the mode's own variables: the TSan runtime also reads UBSAN_OPTIONS,
  # whose log_path would then win over TSAN_OPTIONS'.
  if [ "$mode" = asan ]; then
    echo 'unset TSAN_OPTIONS LSAN_OPTIONS'
    printf 'export ASAN_OPTIONS=%q\n' "detect_leaks=1:abort_on_error=1:log_path=$log_dir/asan"
    printf 'export UBSAN_OPTIONS=%q\n' "print_stacktrace=1:halt_on_error=1:log_path=$log_dir/ubsan"
    if [ -f "$repo/tests/sanitizer/lsan.supp" ]; then
      printf 'export LSAN_OPTIONS=%q\n' "suppressions=$repo/tests/sanitizer/lsan.supp"
    fi
  else
    echo 'unset ASAN_OPTIONS UBSAN_OPTIONS LSAN_OPTIONS'
    printf 'export TSAN_OPTIONS=%q\n' "$tsan"
  fi
  printf 'export SKYCUE_UI_SCREENSHOTS=%q\n' "$build_dir/ui-shots"
  printf 'export SKYCUE_FISB_SCREENSHOTS=%q\n' "$build_dir/ui-shots"
}

if [ "$print_env" = 1 ]; then
  env_lines
  exit 0
fi

# Compiler.
if [ -n "${CXX:-}" ]; then
  cxx=$CXX
elif command -v clang++ >/dev/null 2>&1; then
  cxx=clang++
elif [ "$mode" = asan ] && command -v g++ >/dev/null 2>&1; then
  cxx=g++
elif [ "$mode" = tsan ]; then
  echo "SKIP: clang++ not found (gcc TSan does not run on every kernel)"
  exit 77
else
  echo "SKIP: no C++ compiler found (clang++ or g++)"
  exit 77
fi
if ! command -v "$cxx" >/dev/null 2>&1; then
  echo "run-sanitizers: compiler not found: $cxx" >&2
  exit 1
fi

# Configure once; a later run reuses the tree (cmake --build re-runs cmake
# when a CMakeLists changed).
if [ -f "$build_dir/CMakeCache.txt" ]; then
  have=$(sed -n 's/^SKYCUE_SANITIZE:STRING=//p' "$build_dir/CMakeCache.txt")
  if [ "$have" != "$sanitize" ]; then
    echo "run-sanitizers: $build_dir is configured with SKYCUE_SANITIZE=$have, not $sanitize" >&2
    exit 1
  fi
else
  echo "run-sanitizers: configuring $build_dir ($cxx, -fsanitize=$sanitize)"
  cmake -S "$repo" -B "$build_dir" \
    -DCMAKE_CXX_COMPILER="$cxx" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSKYCUE_BUILD_TESTS=ON \
    -DSKYCUE_UI_TESTS=ON \
    -DSKYCUE_SANITIZE="$sanitize" || exit 1
fi
read_log_dir

# finish RC [LAST_LINE]: --clean's removal, then LAST_LINE, then exit RC.
finish() {
  local rc=$1
  if [ "$clean" = 1 ] && [ ! -f "$build_dir/CMakeCache.txt" ]; then
    echo "run-sanitizers: --clean refused: $build_dir has no CMakeCache.txt" >&2
  elif [ "$clean" = 1 ]; then
    rm -rf "$build_dir"
    echo "run-sanitizers: removed $build_dir"
    df -h "$repo" | tail -n 1
  fi
  [ $# -lt 2 ] || echo "$2"
  exit "$rc"
}

# Prints every log file and sets reports to their count.
collect_logs() {
  reports=0
  local f
  for f in "$log_dir"/*; do
    [ -f "$f" ] || continue
    reports=$((reports + 1))
    echo "==== sanitizer log: ${f#"$build_dir"/}"
    cat "$f"
  done
}

eval "$(env_lines)"
rm -rf "$log_dir"
mkdir -p "$log_dir" "$build_dir/ui-shots"

if [ "$canary" = 1 ]; then
  cmake --build "$build_dir" --target sanitizer_canary -j "$jobs" || finish 1
  "$build_dir/tests/sanitizer_canary"
  collect_logs
  # By report text, not file name: in an ASan + UBSan build both runtimes
  # share one log_path (UBSAN_OPTIONS' wins), so ASan's report lands in a
  # ubsan.<pid> file.
  missing=""
  case "$mode" in
    asan)
      grep -qs 'runtime error:' "$log_dir"/* || missing="$missing ubsan"
      grep -qs 'ERROR: AddressSanitizer' "$log_dir"/* || missing="$missing asan"
      ;;
    tsan)
      grep -qs 'WARNING: ThreadSanitizer' "$log_dir"/* || missing="$missing tsan"
      ;;
  esac
  if [ -z "$missing" ]; then
    finish 0 "canary: detected"
  fi
  finish 1 "canary: NOT detected (no${missing} report in $log_dir)"
fi

cmake --build "$build_dir" -j "$jobs" || finish 1
rm -rf "$log_dir"
mkdir -p "$log_dir"

ctest_out="$build_dir/sanitizer-ctest.log"
ctest --test-dir "$build_dir" --output-on-failure -j "$jobs" "${ctest_args[@]}" 2>&1 | tee "$ctest_out"
ctest_rc=${PIPESTATUS[0]}

collect_logs

summary=$(grep -E '^[0-9]+% tests passed, [0-9]+ tests? failed out of [0-9]+' "$ctest_out" | tail -n 1)
if [ -n "$summary" ]; then
  failed=$(echo "$summary" | sed -E 's/.* ([0-9]+) tests? failed out of.*/\1/')
  tests=$(echo "$summary" | sed -E 's/.* out of ([0-9]+).*/\1/')
else
  tests=0
  failed=$([ "$ctest_rc" = 0 ] && echo 0 || echo "?")
fi

last="sanitizers: $mode: tests=$tests failed=$failed reports=$reports"
if [ "$ctest_rc" = 0 ] && [ "$reports" = 0 ]; then
  finish 0 "$last"
fi
finish 1 "$last"
