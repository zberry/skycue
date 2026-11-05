#!/usr/bin/env bash
# SKYCUE_TEST_SLOW_FACTOR in a plain (not sanitizer, not fuzz) build:
# configures skycue/ twice into a temporary directory (configure only, no
# build) and reads the registered tests back with `ctest --show-only=json-v1`.
#   - factor 3: every test's TIMEOUT is 3x its own (1500 s when it sets
#     none) and its ENVIRONMENT has SKYCUE_TEST_SLOW=3;
#   - no factor: timeouts are the tests' own and no SKYCUE_TEST_SLOW is set.
#
# usage: test_slow_factor.sh <skycue dir> <work dir> <python3>
# Environment: CMAKE, CTEST (default: cmake/ctest on PATH), CXX,
# SKYCUE_SMOKE_BOOST_DIR, SKYCUE_SMOKE_PREFIX_PATH (as the enclosing build).
set -euo pipefail

skycue_dir=$1
work_base=$2
python3_bin=$3
cmake_bin=${CMAKE:-cmake}
ctest_bin=${CTEST:-ctest}

mkdir -p "$work_base"
work=$(mktemp -d "$work_base/slow-factor.XXXXXX")
trap 'rm -rf "$work"' EXIT

args=(-DSKYCUE_UI_TESTS=OFF)
if [[ -n ${CXX:-} ]]; then args+=("-DCMAKE_CXX_COMPILER=$CXX"); fi
if [[ -n ${SKYCUE_SMOKE_BOOST_DIR:-} ]]; then args+=("-DBoost_DIR=$SKYCUE_SMOKE_BOOST_DIR"); fi
if [[ -n ${SKYCUE_SMOKE_PREFIX_PATH:-} ]]; then args+=("-DCMAKE_PREFIX_PATH=$SKYCUE_SMOKE_PREFIX_PATH"); fi

"$cmake_bin" -S "$skycue_dir" -B "$work/plain" "${args[@]}" >/dev/null
"$cmake_bin" -S "$skycue_dir" -B "$work/slow" "${args[@]}" -DSKYCUE_TEST_SLOW_FACTOR=3 >/dev/null
"$ctest_bin" --test-dir "$work/plain" --show-only=json-v1 >"$work/plain.json"
"$ctest_bin" --test-dir "$work/slow" --show-only=json-v1 >"$work/slow.json"

"$python3_bin" -I - "$work/plain.json" "$work/slow.json" <<'EOF'
import json, sys

def load(path):
    tests = {}
    for t in json.load(open(path))["tests"]:
        props = {p["name"]: p["value"] for p in t.get("properties", [])}
        timeout = props.get("TIMEOUT")
        env = props.get("ENVIRONMENT", [])
        slow = [e for e in env if e.startswith("SKYCUE_TEST_SLOW=")]
        tests[t["name"]] = (None if timeout is None else float(timeout), slow)
    return tests

plain, slow = load(sys.argv[1]), load(sys.argv[2])
errors = []
if set(plain) != set(slow):
    errors.append("test sets differ: %s" % sorted(set(plain) ^ set(slow)))
if len(plain) < 10:
    errors.append("only %d tests registered" % len(plain))
for name in sorted(set(plain) & set(slow)):
    p_timeout, p_slow = plain[name]
    s_timeout, s_slow = slow[name]
    if p_slow:
        errors.append("%s: SKYCUE_TEST_SLOW set without a factor: %s" % (name, p_slow))
    want = 3 * (p_timeout if p_timeout else 1500.0)
    if s_timeout != want:
        errors.append("%s: TIMEOUT %s with factor 3, want %s (plain %s)" % (name, s_timeout, want, p_timeout))
    if s_slow != ["SKYCUE_TEST_SLOW=3"]:
        errors.append("%s: ENVIRONMENT has %s with factor 3" % (name, s_slow))
# Spot checks on known values, so a change that broke both runs alike shows.
if plain.get("web_server", (None,))[0] != 120.0:
    errors.append("web_server plain TIMEOUT %s, want 120" % (plain.get("web_server"),))
if slow.get("web_server", (None,))[0] != 360.0:
    errors.append("web_server TIMEOUT %s with factor 3, want 360" % (slow.get("web_server"),))
if errors:
    print("\n".join(errors), file=sys.stderr)
    sys.exit(1)
print("slow factor: %d tests scaled by 3, unscaled without a factor" % len(slow))
EOF
