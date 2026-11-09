#!/usr/bin/env bash
# The packaged skycue.toml (cmake/PackageConfig.cmake run on the example):
# every package-shape line is there exactly as the packages need it and every
# other line is the example's own; skycued started on it before the receiver
# position is set stops with exit status 2 and one line naming the file and
# the keys (what `journalctl -u skycued` shows); with the position set it parses
# as a whole (--check-inputs gets as far as probing the inputs); and an example
# that no longer has a line the script edits makes the script fail, naming it.
# usage: test_package_conf.sh CMAKE PACKAGE_CONFIG_CMAKE EXAMPLE SKYCUED WORK_DIR PYTHON3
#   PYTHON3: the interpreter that picks a closed port; when it is empty or
#   not there (SKYCUE_PYTHON3-NOTFOUND) the test is skipped with exit 77.
set -u
cmake=$1 script=$2 example=$3 skycued=$4 work=$5 python3=${6:-}
fail() { echo "FAIL: $*"; exit 1; }
[ -n "$python3" ] && [ -x "$python3" ] || { echo "SKIP: python3 not found"; exit 77; }
for f in "$script" "$example" "$skycued"; do [ -e "$f" ] || fail "$f missing"; done
rm -rf "$work"
mkdir -p "$work" || fail "cannot create $work"
out=$work/skycue.toml

"$cmake" -DIN="$example" -DOUT="$out" -P "$script" >"$work/gen.log" 2>&1 \
  || { cat "$work/gen.log"; fail "PackageConfig.cmake failed on the example"; }
[ -s "$out" ] || fail "no $out"

# The edited lines, exactly.
# The example's trailing comments stay, so all but the header and state_dir
# are matched up to the comment.
expected=(
  "# skycue configuration (packaged). Set receiver lat and lon, then: systemctl enable --now skycued"
  'state_dir = "/var/lib/skycue"'
  'map_dir = "/var/lib/skycue/maps"'
  '#dir = "/var/lib/skycue/recordings"'
  'allow_list_file = "allow.list"'
  'block_list_file = "block.list"'
  'allow_restart = true'
)
[ "$(grep -cxF -- "${expected[0]}" "$out")" = 1 ] || fail "$out: want exactly one line '${expected[0]}'"
[ "$(grep -cxF -- "${expected[1]}" "$out")" = 1 ] || fail "$out: want exactly one line '${expected[1]}'"
for line in "${expected[@]:2}"; do
  [ "$(grep -cE -- "^$line( +#.*)?\$" "$out")" = 1 ] || fail "$out: want exactly one line '$line'"
done
[ "$(head -n 1 "$out")" = "${expected[0]}" ] || fail "$out: line 1 is not the packaged header"
# Left as they are: the receiver position (0.0), the port defaults.
for line in 'lat = 0.0' 'lon = 0.0' 'raw_port = 30978' 'json_port = 30047' \
            'tcp_port = 30500' 'port = 8080'; do
  [ "$(grep -cE -- "^$line( +#.*)?\$" "$out")" = 1 ] || fail "$out: want '$line' kept from the example"
done
# Nothing but those seven lines differs from the example.
[ "$(wc -l <"$out")" = "$(wc -l <"$example")" ] || fail "$out and the example differ in line count"
changed=$(paste -d '\n' "$example" "$out" | awk 'NR % 2 { a = $0; next } a != $0 { n++ } END { print n + 0 }')
[ "$changed" = "${#expected[@]}" ] || fail "$changed lines differ from the example, want ${#expected[@]}"
# No key is left with a relative or empty state location.
grep -qE '^(state_dir|map_dir) = ""' "$out" && fail "$out: state_dir or map_dir left empty"

# Started before the position is set: exit 2, one line on stderr, naming the
# file and the key.
"$skycued" --config "$out" --check-inputs >"$work/check.out" 2>"$work/check.err"
rc=$?
[ "$rc" = 2 ] || { cat "$work/check.err"; fail "unset receiver.lat: exit $rc, want 2"; }
[ "$(wc -l <"$work/check.err")" = 1 ] || { cat "$work/check.err"; fail "want one line on stderr"; }
latline=$(grep -n '^lat = ' "$out" | head -n 1 | cut -d: -f1)
[ -n "$latline" ] || fail "$out has no lat = line"
grep -qxF "skycued: $out:$latline: receiver.lat and receiver.lon are both 0.0: set the receiver's position" "$work/check.err" \
  || { cat "$work/check.err"; fail "stderr is not the missing-position line naming $out"; }

# Position set, state under the work dir, both decoder ports closed: the file
# parses as a whole and the probes run (exit 1, the readsb line).
closed=$("$python3" -I -c 'import socket
s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
mkdir -p "$work/state" "$work/maps"
sed -e "s|^state_dir = .*|state_dir = \"$work/state\"|" -e "s|^map_dir = .*|map_dir = \"$work/maps\"|" \
    -e "s|^json_port = 30047.*|json_port = $closed|" \
    -e "s|^raw_port = 30978.*|raw_port = $closed|" \
    -e "s|^lat = 0.0.*|lat = 38.9|" -e "s|^lon = 0.0.*|lon = -77.0|" "$out" >"$work/set.toml"
"$skycued" --config "$work/set.toml" --check-inputs >"$work/set.out" 2>"$work/set.err"
rc=$?
[ "$rc" = 1 ] || { cat "$work/set.out" "$work/set.err"; fail "position set: exit $rc, want 1"; }
grep -q "^readsb 127.0.0.1:$closed: " "$work/set.out" \
  || { cat "$work/set.out" "$work/set.err"; fail "no readsb 127.0.0.1:$closed line"; }

# An example without a line the script edits: the script fails and names it.
grep -v '^state_dir = ' "$example" >"$work/drifted.example"
if "$cmake" -DIN="$work/drifted.example" -DOUT="$work/drifted.toml" -P "$script" \
     >"$work/drifted.log" 2>&1; then
  fail "PackageConfig.cmake accepted an example without state_dir"
fi
grep -q 'state_dir' "$work/drifted.log" || { cat "$work/drifted.log"; fail "the error does not name state_dir"; }
[ -e "$work/drifted.toml" ] && fail "a failed run still wrote its output"
# ... and one with a line twice.
{ cat "$example"; printf '[paths]\nmap_dir = ""\n'; } >"$work/twice.example"
if "$cmake" -DIN="$work/twice.example" -DOUT="$work/twice.toml" -P "$script" \
     >"$work/twice.log" 2>&1; then
  fail "PackageConfig.cmake accepted an example with map_dir twice"
fi
grep -q 'map_dir' "$work/twice.log" || { cat "$work/twice.log"; fail "the error does not name map_dir"; }

rm -rf "$work"
echo "PASS"
