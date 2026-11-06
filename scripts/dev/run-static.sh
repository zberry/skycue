#!/usr/bin/env bash
# Static analysis of skycue's own sources: clang-tidy (.clang-tidy)
# and cppcheck (.cppcheck-suppressions).
#
#   scripts/dev/run-static.sh [--build-dir DIR] [-j N] [--tidy-only | --cppcheck-only]
#
# The tree is configured (not built: nothing is generated) into DIR, default
# <repo>/build-static, with clang++ and CMAKE_EXPORT_COMPILE_COMMANDS=ON; the
# files analysed are the translation units in its compile_commands.json under
# skycued, libskycue and tools/ (not tests/, which holds
# tests/fuzz/). -j defaults to the number of cores. DIR is run-static's own tree:
# a directory other than <repo>/build-static that already holds a
# CMakeCache.txt without the marker file .skycue-static-tree (another build,
# such as the one ctest runs from) is refused, never reconfigured.
#
#   clang-tidy: run-clang-tidy -p DIR -j N <files>; the warnings (and errors)
#               in skycue's own files are counted once each.
#   cppcheck:   --project=DIR/compile_commands.json --enable=warning,portability
#               --std=c++17, the suppressions list, tests/ ignored.
#
# Each tool's findings are printed, then one summary line, always the last:
#   static: clang-tidy=<warnings|SKIP> cppcheck=<warnings|SKIP>
# A tool that is not installed (or not selected) is SKIP; the other still runs.
#
# Exit status: 0 no warnings, 1 a warning (or a tool or the configure failed),
# 2 usage, 77 skipped (none of the selected tools is installed).
set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)

usage() {
  sed -n '5p' "${BASH_SOURCE[0]}" | sed 's/^# *//' >&2
  exit 2
}

abs() {
  case "$1" in
    /*) echo "$1" ;;
    *) echo "$PWD/$1" ;;
  esac
}

build_dir="$repo/build-static"
jobs=$(nproc 2>/dev/null || echo 4)
want_tidy=1
want_cppcheck=1
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) [ $# -ge 2 ] || usage; build_dir=$(abs "$2"); shift 2 ;;
    -j) [ $# -ge 2 ] || usage; jobs=$2; shift 2 ;;
    --tidy-only) want_cppcheck=0; shift ;;
    --cppcheck-only) want_tidy=0; shift ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
done
case "$jobs" in ''|*[!0-9]*|0) usage ;; esac
[ "$want_tidy$want_cppcheck" != 00 ] || usage

tidy=SKIP
cppcheck=SKIP
status=0
summary() {
  echo "static: clang-tidy=$tidy cppcheck=$cppcheck"
  exit "$status"
}

have_tidy=0
have_cppcheck=0
if [ "$want_tidy" = 1 ]; then
  if command -v clang-tidy >/dev/null && command -v run-clang-tidy >/dev/null; then
    have_tidy=1
  else
    echo "SKIP: clang-tidy: clang-tidy or run-clang-tidy not found"
  fi
fi
if [ "$want_cppcheck" = 1 ]; then
  if command -v cppcheck >/dev/null; then
    have_cppcheck=1
  else
    echo "SKIP: cppcheck: cppcheck not found"
  fi
fi
if [ "$have_tidy$have_cppcheck" = 00 ]; then
  status=77
  summary
fi

# Configure only. clang++ gives clang-tidy the command lines it understands
# best; without it (cppcheck alone) the default compiler does.
cxx=${CXX:-clang++}
command -v "$cxx" >/dev/null || cxx=c++
marker="$build_dir/.skycue-static-tree"
if [ -f "$build_dir/CMakeCache.txt" ] && [ ! -f "$marker" ] \
    && [ "$build_dir" != "$repo/build-static" ]; then
  echo "static: $build_dir is another build tree (no .skycue-static-tree); not reconfiguring it" >&2
  status=1
  summary
fi
echo "static: configuring $build_dir with $cxx"
mkdir -p "$build_dir" || { status=1; summary; }
: >"$marker" || { status=1; summary; }
if ! cmake -S "$repo" -B "$build_dir" -DCMAKE_CXX_COMPILER="$cxx" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSKYCUE_UI_TESTS=OFF >"$build_dir/configure.log" 2>&1; then
  cat "$build_dir/configure.log" >&2
  echo "static: configure failed" >&2
  status=1
  summary
fi
db="$build_dir/compile_commands.json"
[ -f "$db" ] || { echo "static: no $db" >&2; status=1; summary; }

# The translation units to analyse, one per line.
mapfile -t files < <(python3 -I - "$db" "$repo" <<'EOF'
import json, os, re, sys
db, src = sys.argv[1], os.path.realpath(sys.argv[2])
own = re.compile(re.escape(src) + r"/(skycued|libskycue|tools)/")
seen = set()
for e in json.load(open(db)):
    f = os.path.realpath(os.path.join(e["directory"], e["file"]))
    if own.match(f) and f not in seen:
        seen.add(f)
        print(f)
EOF
)
if [ "${#files[@]}" -eq 0 ]; then
  echo "static: no skycue sources in $db" >&2
  status=1
  summary
fi
echo "static: ${#files[@]} translation units"

# count_findings LOG REGEX: the distinct "<file>:<line>:<col>: <kind>: ..."
# lines in LOG whose file is under the source tree (each header finding is reported
# once per translation unit that includes it), printed and counted.
count_findings() {
  python3 -I - "$1" "$repo" "$2" <<'EOF'
import os, re, sys
log, src, kinds = sys.argv[1], os.path.realpath(sys.argv[2]), sys.argv[3]
line = re.compile(r"^(/[^:]+):(\d+):(\d+): (" + kinds + r"): (.*)$")
seen = []
for l in open(log, errors="replace"):
    m = line.match(l.rstrip("\n"))
    if not m:
        continue
    f = os.path.realpath(m.group(1))
    if not f.startswith(src + "/"):
        continue
    key = (f, int(m.group(2)), int(m.group(3)), m.group(5))
    if key not in seen:
        seen.append(key)
for f, ln, col, msg in sorted(seen):
    print(f"{os.path.relpath(f, os.path.dirname(src))}:{ln}:{col}: {msg}", file=sys.stderr)
print(len(seen))
EOF
}

if [ "$have_tidy" = 1 ]; then
  log="$build_dir/clang-tidy.log"
  regex=$(printf '%s\n' "${files[@]}" | python3 -I -c \
    'import re, sys; print("|".join("^" + re.escape(l.strip()) + "$" for l in sys.stdin if l.strip()))')
  echo "static: clang-tidy on ${#files[@]} files, -j $jobs (log $log)"
  run-clang-tidy -quiet -p "$build_dir" -j "$jobs" "$regex" >"$log" 2>&1
  rc=$?
  tidy=$(count_findings "$log" "warning|error")
  if [ -z "$tidy" ]; then
    echo "static: clang-tidy: could not read $log" >&2
    tidy=1
  fi
  [ "$tidy" = 0 ] || status=1
  if [ "$rc" != 0 ] && [ "$tidy" = 0 ]; then
    tail -n 20 "$log" >&2
    echo "static: run-clang-tidy exited $rc with no finding in skycue's files" >&2
    status=1
  fi
fi

if [ "$have_cppcheck" = 1 ]; then
  log="$build_dir/cppcheck.log"
  echo "static: cppcheck, -j $jobs (log $log)"
  cppcheck --project="$db" --enable=warning,portability --std=c++17 \
    --suppressions-list="$repo/.cppcheck-suppressions" \
    --error-exitcode=1 -i "$repo/tests" \
    -j "$jobs" --quiet \
    --template='{file}:{line}:{column}: {severity}: {message} [{id}]' >"$log" 2>&1
  rc=$?
  cppcheck=$(count_findings "$log" "error|warning|portability|style|performance|information")
  if [ -z "$cppcheck" ]; then
    echo "static: cppcheck: could not read $log" >&2
    cppcheck=1
  fi
  [ "$cppcheck" = 0 ] || status=1
  if [ "$rc" != 0 ] && [ "$cppcheck" = 0 ]; then
    tail -n 20 "$log" >&2
    echo "static: cppcheck exited $rc with no finding in skycue's files" >&2
    status=1
  fi
fi

summary
