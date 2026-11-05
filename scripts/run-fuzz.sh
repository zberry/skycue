#!/usr/bin/env bash
# Builds the libFuzzer harnesses in fuzz and runs them.
#
#   scripts/run-fuzz.sh [TARGET|all] [--seconds N] [--build-dir DIR] [--bin-dir DIR]
#                       [--work DIR] [--findings DIR] [--seeds DIR] [--fixtures DIR]
#                       [--canary] [--clean]
#
# TARGET is a harness name (readsb_line, ...; fuzz_<name> in the tree) or all
# (the default). Defaults: --seconds 600, --build-dir <repo>/build-fuzz,
# --work <build>/fuzz-work, --findings <repo>/fuzz/findings.
#
# Capture-derived seeds are not committed. When the fetched captures are in
# --fixtures (default <repo>/tests/fixtures), scripts/gen-capture-seeds.py
# writes them to <work>/capture-seeds/<target>/ and they are copied into the
# target's fresh corpus beside the committed seeds; without the captures a
# "SKIP: generated seeds for <target>" line says so and the run goes on with
# the committed seeds. Every target needs at least one committed seed
# (--seeds DIR, default <repo>/fuzz/seeds): none is an error.
#
# Without --bin-dir the tree is configured once with clang++ and
# -DSKYCUE_FUZZ=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo and only the fuzz_*
# targets are built; --bin-dir DIR runs the fuzz_* executables already in DIR
# (ctest fuzz_smoke does this).
#
# Each target runs on the corpus <work>/corpus/<target> (its seeds from
# fuzz/seeds/<target>/ copied in when empty, then the capture-derived seeds
# below for the targets that have them) with artefacts in
# <work>/artifacts/<target>/ (emptied first); each artefact is copied to
# <findings>/<target>-<name>. Every harness gets SKYCUE_FUZZ_WORK=<work> (a
# harness that needs files makes them under $SKYCUE_FUZZ_WORK/<target>/),
# SKYCUE_TEST_SLOW=4 and the ASan + UBSan options of
# scripts/run-sanitizers.sh --print-env asan, their log_path under
# <work>/sanitizer-logs; libFuzzer itself still reports on stderr. One line
# per target:
#   fuzz: <target> seconds=N execs=<E> corpus=<C> findings=<F>
# also appended to <work>/summary.tsv.
#
#   --canary  run on a scratch corpus holding an extra "skycue-canary" seed,
#             with SKYCUE_FUZZ_CANARY=1: the harness aborts on it, so a
#             working setup reports findings=1 and exits 1.
#   --clean   remove the work dir and (when the script built it) the build
#             tree afterwards; refused for a build dir without CMakeCache.txt.
#             Without a TARGET, cleans only and runs nothing.
#
# Exit status: 0 no findings, 1 findings or a harness that failed, 2 usage,
# 77 skipped (no clang++ or no libFuzzer).
set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)

usage() {
  sed -n '4,5p' "${BASH_SOURCE[0]}" | sed 's/^# *//' >&2
  exit 2
}

abs() {
  case "$1" in
    /*) echo "$1" ;;
    *) echo "$PWD/$1" ;;
  esac
}

target=""
seconds=600
build_dir=""
bin_dir=""
work=""
findings=""
seeds_root=""
fixtures=""
canary=0
clean=0

while [ $# -gt 0 ]; do
  case "$1" in
    --seconds) shift; [ $# -gt 0 ] || usage; seconds=$1 ;;
    --build-dir) shift; [ $# -gt 0 ] || usage; build_dir=$(abs "$1") ;;
    --bin-dir) shift; [ $# -gt 0 ] || usage; bin_dir=$(abs "$1") ;;
    --work) shift; [ $# -gt 0 ] || usage; work=$(abs "$1") ;;
    --findings) shift; [ $# -gt 0 ] || usage; findings=$(abs "$1") ;;
    --seeds) shift; [ $# -gt 0 ] || usage; seeds_root=$(abs "$1") ;;
    --fixtures) shift; [ $# -gt 0 ] || usage; fixtures=$(abs "$1") ;;
    --canary) canary=1 ;;
    --clean) clean=1 ;;
    -h|--help) usage ;;
    -*) echo "run-fuzz: unknown option: $1" >&2; usage ;;
    *) [ -z "$target" ] || usage; target=$1 ;;
  esac
  shift
done

case "$seconds" in
  ""|*[!0-9]*|0) echo "run-fuzz: --seconds needs a positive number" >&2; exit 2 ;;
esac
[ -n "$build_dir" ] || build_dir="$repo/build-fuzz"
[ -n "$work" ] || work="$build_dir/fuzz-work"
[ -n "$findings" ] || findings="$repo/fuzz/findings"
[ -n "$seeds_root" ] || seeds_root="$repo/fuzz/seeds"
[ -n "$fixtures" ] || fixtures="$repo/tests/fixtures"

# Refuse --clean on a build dir that is not a CMake tree before doing anything,
# so a mistyped --build-dir is never deleted.
if [ "$clean" = 1 ] && [ -z "$bin_dir" ] && [ -e "$build_dir" ] && [ ! -f "$build_dir/CMakeCache.txt" ]; then
  echo "run-fuzz: --clean refused: $build_dir has no CMakeCache.txt" >&2
  exit 2
fi

do_clean() {
  case "$work" in
    /|"$repo"|"$repo/"|"$HOME"|"$HOME/")
      echo "run-fuzz: --clean refused: work dir $work" >&2
      return
      ;;
  esac
  rm -rf "$work"
  echo "run-fuzz: removed $work"
  if [ -z "$bin_dir" ] && [ -f "$build_dir/CMakeCache.txt" ]; then
    rm -rf "$build_dir"
    echo "run-fuzz: removed $build_dir"
  fi
  df -h "$repo" | tail -n 1
}

if [ "$clean" = 1 ] && [ -z "$target" ]; then
  do_clean
  exit 0
fi
[ -n "$target" ] || target=all

jobs=$(nproc 2>/dev/null || echo 2)

if [ -z "$bin_dir" ]; then
  cxx=clang++
  if ! command -v "$cxx" >/dev/null 2>&1; then
    echo "SKIP: clang++ not found (fuzz harnesses need clang)"
    exit 77
  fi
  # One-line probe: can this clang link a libFuzzer binary?
  probe=$(mktemp -d)
  printf '%s\n' '#include <cstddef>' '#include <cstdint>' \
    'extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, std::size_t) { return 0; }' \
    > "$probe/probe.cpp"
  if ! "$cxx" -fsanitize=fuzzer "$probe/probe.cpp" -o "$probe/probe" 2> "$probe/err"; then
    first=$(grep -m 1 -i 'error' "$probe/err" || head -n 1 "$probe/err")
    rm -rf "$probe"
    echo "SKIP: libFuzzer not available ($first)"
    exit 77
  fi
  rm -rf "$probe"

  if [ -f "$build_dir/CMakeCache.txt" ]; then
    have=$(sed -n 's/^SKYCUE_FUZZ:BOOL=//p' "$build_dir/CMakeCache.txt")
    if [ "$have" != ON ]; then
      echo "run-fuzz: $build_dir is not configured with SKYCUE_FUZZ=ON" >&2
      exit 1
    fi
  else
    echo "run-fuzz: configuring $build_dir ($cxx, -DSKYCUE_FUZZ=ON)"
    cmake -S "$repo" -B "$build_dir" \
      -DCMAKE_CXX_COMPILER="$cxx" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DSKYCUE_FUZZ=ON || exit 1
  fi
  bin_dir="$build_dir/fuzz"
  if [ ! -f "$bin_dir/targets.txt" ]; then
    echo "run-fuzz: $bin_dir/targets.txt missing (configure did not add fuzz)" >&2
    exit 1
  fi
  build_targets=()
  while IFS= read -r n; do
    [ -n "$n" ] && build_targets+=("fuzz_$n")
  done < "$bin_dir/targets.txt"
  cmake --build "$build_dir" -j "$jobs" --target "${build_targets[@]}" || exit 1
fi

# The harnesses in bin_dir: targets.txt (written by fuzz/CMakeLists.txt),
# else every fuzz_* executable there.
known=()
if [ -f "$bin_dir/targets.txt" ]; then
  while IFS= read -r n; do
    [ -n "$n" ] && known+=("$n")
  done < "$bin_dir/targets.txt"
else
  for f in "$bin_dir"/fuzz_*; do
    [ -f "$f" ] && [ -x "$f" ] && known+=("${f##*/fuzz_}")
  done
fi
if [ ${#known[@]} -eq 0 ]; then
  echo "run-fuzz: no fuzz harnesses in $bin_dir" >&2
  exit 1
fi
if [ "$target" = all ]; then
  targets=("${known[@]}")
else
  found=0
  for n in "${known[@]}"; do
    [ "$n" = "$target" ] && found=1
  done
  if [ "$found" = 0 ]; then
    echo "run-fuzz: unknown target $target (have: ${known[*]})" >&2
    exit 2
  fi
  targets=("$target")
fi

# The environment every harness runs with (H3, H5).
mkdir -p "$work" "$findings"
san_env=$(bash "$repo/scripts/run-sanitizers.sh" --print-env asan --build-dir "$work") || exit 1
eval "$san_env"
log_dir="$work/sanitizer-logs"
export SKYCUE_FUZZ_WORK="$work"
export SKYCUE_TEST_SLOW=4
if [ "$canary" = 1 ]; then
  export SKYCUE_FUZZ_CANARY=1
else
  unset SKYCUE_FUZZ_CANARY
fi

# Seeds cut from the fetched captures (scripts/gen-capture-seeds.py); exit 3
# means some capture is missing and that target gets none; any other exit
# status is a generator failure.
capture_targets=" raw_formats fisb_decoder iqgen_coding recording "
gen_dir="$work/capture-seeds"
rm -rf "$gen_dir"
gen_failed=0
if [ -d "$fixtures" ]; then
  python3 -I "$repo/scripts/gen-capture-seeds.py" "$fixtures" "$gen_dir"
  gen_rc=$?
  case "$gen_rc" in
    0 | 3) ;;
    *) echo "run-fuzz: gen-capture-seeds.py failed with exit $gen_rc" >&2; gen_failed=1 ;;
  esac
else
  echo "run-fuzz: no fixtures dir $fixtures" >&2
fi

summary="$work/summary.tsv"
[ -f "$summary" ] || printf 'target\tseconds\texecs\tcorpus\tfindings\n' > "$summary"

status=$gen_failed
for t in "${targets[@]}"; do
  bin="$bin_dir/fuzz_$t"
  if [ ! -x "$bin" ]; then
    echo "run-fuzz: $bin not found" >&2
    status=1
    continue
  fi
  seeds="$seeds_root/$t"
  gen_seeds="$gen_dir/$t"
  if [ -z "$(ls -A "$seeds" 2>/dev/null)" ]; then
    echo "run-fuzz: $seeds has no committed seed" >&2
    status=1
    continue
  fi
  case "$capture_targets" in
    *" $t "*)
      if [ ! -d "$gen_seeds" ]; then
        echo "SKIP: generated seeds for $t (captures not fetched or incomplete); using the committed seeds only"
      fi
      ;;
  esac
  if [ "$canary" = 1 ]; then
    corpus="$work/canary-corpus/$t"
    rm -rf "$corpus"
    mkdir -p "$corpus"
    cp "$seeds"/* "$corpus"/
    [ -d "$gen_seeds" ] && cp "$gen_seeds"/* "$corpus"/
    printf 'skycue-canary\n' > "$corpus/seed-0.txt"
  else
    corpus="$work/corpus/$t"
    mkdir -p "$corpus"
    if [ -z "$(ls -A "$corpus")" ]; then
      cp "$seeds"/* "$corpus"/
      [ -d "$gen_seeds" ] && cp "$gen_seeds"/* "$corpus"/
    fi
  fi
  artifacts="$work/artifacts/$t"
  rm -rf "$artifacts"
  mkdir -p "$artifacts" "$work/$t" "$work/logs"
  rm -rf "$log_dir"
  mkdir -p "$log_dir"
  case "$t" in
    web|pingstation|config_files) max_len=65600 ;;
    *) max_len=4096 ;;
  esac

  log="$work/logs/$t.log"
  echo "run-fuzz: $t for ${seconds}s (corpus $corpus, log $log)"
  "$bin" -max_total_time="$seconds" -timeout=10 -rss_limit_mb=2048 \
    -max_len="$max_len" -print_final_stats=1 \
    -artifact_prefix="$artifacts/" "$corpus" > "$log" 2>&1
  rc=$?

  execs=$(sed -n 's/^stat::number_of_executed_units: *//p' "$log" | tail -n 1)
  [ -n "$execs" ] || execs=0
  ncorpus=$(find "$corpus" -type f | wc -l)
  nfound=0
  for a in "$artifacts"/*; do
    [ -f "$a" ] || continue
    nfound=$((nfound + 1))
    cp "$a" "$findings/$t-${a##*/}"
    echo "run-fuzz: finding $findings/$t-${a##*/}"
  done

  if [ "$nfound" -gt 0 ] || [ "$rc" != 0 ]; then
    tail -n 40 "$log"
    for f in "$log_dir"/*; do
      [ -f "$f" ] || continue
      echo "==== sanitizer log: ${f#"$work"/}"
      cat "$f"
    done
  fi
  if [ "$nfound" -gt 0 ]; then
    status=1
  elif [ "$rc" != 0 ]; then
    echo "run-fuzz: fuzz_$t exited $rc without an artefact" >&2
    status=1
  fi

  echo "fuzz: $t seconds=$seconds execs=$execs corpus=$ncorpus findings=$nfound"
  printf '%s\t%s\t%s\t%s\t%s\n' "$t" "$seconds" "$execs" "$ncorpus" "$nfound" >> "$summary"
done

if [ "$clean" = 1 ]; then
  do_clean
fi
exit "$status"
