#!/usr/bin/env bash
# The packaged skycue.conf (cmake/PackageConfig.cmake run on the example):
# every package-shape line is there exactly as the packages need it and every
# other line is the example's own; skycued started on it before the receiver
# position is set stops with exit status 2 and one line naming the file and
# the key (what `journalctl -u skycued` shows); with the position set it parses
# as a whole (--check-inputs gets as far as probing the inputs); and an example
# that no longer has a line the script edits makes the script fail, naming it.
# usage: test_package_conf.sh CMAKE PACKAGE_CONFIG_CMAKE EXAMPLE SKYCUED WORK_DIR
set -u
cmake=$1 script=$2 example=$3 skycued=$4 work=$5
fail() { echo "FAIL: $*"; exit 1; }
for f in "$script" "$example" "$skycued"; do [ -e "$f" ] || fail "$f missing"; done
rm -rf "$work"
mkdir -p "$work" || fail "cannot create $work"
out=$work/skycue.conf

"$cmake" -DIN="$example" -DOUT="$out" -P "$script" >"$work/gen.log" 2>&1 \
  || { cat "$work/gen.log"; fail "PackageConfig.cmake failed on the example"; }
[ -s "$out" ] || fail "no $out"

# The edited lines, exactly.
expected=(
  "# skycue configuration (packaged). Set RECEIVER_LAT and RECEIVER_LON, then: systemctl start skycued"
  "STATE_DIR=/var/lib/skycue"
  "MAP_DIR=/var/lib/skycue/maps"
  "#RECORD_DIR=/var/lib/skycue/recordings"
  "ALLOW_LIST_FILE=allow.list"
  "BLOCK_LIST_FILE=block.list"
  "#SDR_1090_SERIAL="
  "#SDR_978_SERIAL="
  "#GAIN_1090=auto"
  "#GAIN_978=auto"
)
for line in "${expected[@]}"; do
  [ "$(grep -cxF -- "$line" "$out")" = 1 ] || fail "$out: want exactly one line '$line'"
done
[ "$(head -n 1 "$out")" = "${expected[0]}" ] || fail "$out: line 1 is not the packaged header"
# Left as they are: the receiver position (empty), the port defaults.
for line in RECEIVER_LAT= RECEIVER_LON= DUMP978_RAW_PORT=30978 READSB_JSON_PORT=30047 \
            CUE_TCP_PORT=30500 WEB_PORT=8080; do
  [ "$(grep -cxF -- "$line" "$out")" = 1 ] || fail "$out: want '$line' kept from the example"
done
# Nothing but those ten lines differs from the example.
[ "$(wc -l <"$out")" = "$(wc -l <"$example")" ] || fail "$out and the example differ in line count"
changed=$(paste -d '\n' "$example" "$out" | awk 'NR % 2 { a = $0; next } a != $0 { n++ } END { print n + 0 }')
[ "$changed" = "${#expected[@]}" ] || fail "$changed lines differ from the example, want ${#expected[@]}"
# No key is left with a relative or empty state location.
grep -qE '^(STATE_DIR|MAP_DIR)=$' "$out" && fail "$out: STATE_DIR or MAP_DIR left empty"

# Started before the position is set: exit 2, one line on stderr, naming the
# file and the key.
"$skycued" --config "$out" --check-inputs >"$work/check.out" 2>"$work/check.err"
rc=$?
[ "$rc" = 2 ] || { cat "$work/check.err"; fail "unset RECEIVER_LAT: exit $rc, want 2"; }
[ "$(wc -l <"$work/check.err")" = 1 ] || { cat "$work/check.err"; fail "want one line on stderr"; }
grep -qxF "skycued: $out: missing required key RECEIVER_LAT" "$work/check.err" \
  || { cat "$work/check.err"; fail "stderr does not name $out and RECEIVER_LAT"; }

# Position set, state under the work dir, both decoder ports closed: the file
# parses as a whole and the probes run (exit 1, the readsb line).
closed=$(python3 -I -c 'import socket
s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
mkdir -p "$work/state" "$work/maps"
sed -e "s|^STATE_DIR=.*|STATE_DIR=$work/state|" -e "s|^MAP_DIR=.*|MAP_DIR=$work/maps|" \
    -e "s|^READSB_JSON_PORT=.*|READSB_JSON_PORT=$closed|" \
    -e "s|^DUMP978_RAW_PORT=.*|DUMP978_RAW_PORT=$closed|" "$out" >"$work/set.conf"
printf 'RECEIVER_LAT=38.9\nRECEIVER_LON=-77.0\n' >>"$work/set.conf"
"$skycued" --config "$work/set.conf" --check-inputs >"$work/set.out" 2>"$work/set.err"
rc=$?
[ "$rc" = 1 ] || { cat "$work/set.out" "$work/set.err"; fail "position set: exit $rc, want 1"; }
grep -q "^readsb 127.0.0.1:$closed: " "$work/set.out" \
  || { cat "$work/set.out" "$work/set.err"; fail "no readsb 127.0.0.1:$closed line"; }

# An example without a line the script edits: the script fails and names it.
grep -v '^STATE_DIR=' "$example" >"$work/drifted.example"
if "$cmake" -DIN="$work/drifted.example" -DOUT="$work/drifted.conf" -P "$script" \
     >"$work/drifted.log" 2>&1; then
  fail "PackageConfig.cmake accepted an example without STATE_DIR="
fi
grep -q 'STATE_DIR=' "$work/drifted.log" || { cat "$work/drifted.log"; fail "the error does not name STATE_DIR="; }
[ -e "$work/drifted.conf" ] && fail "a failed run still wrote its output"
# ... and one with a line twice.
{ cat "$example"; echo "MAP_DIR="; } >"$work/twice.example"
if "$cmake" -DIN="$work/twice.example" -DOUT="$work/twice.conf" -P "$script" \
     >"$work/twice.log" 2>&1; then
  fail "PackageConfig.cmake accepted an example with MAP_DIR= twice"
fi
grep -q 'MAP_DIR=' "$work/twice.log" || { cat "$work/twice.log"; fail "the error does not name MAP_DIR="; }

rm -rf "$work"
echo "PASS"
