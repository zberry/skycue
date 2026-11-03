#!/usr/bin/env bash
# skycue-pingsim end to end: a skycued with only a pingstation_cat021 UDP
# input (capture on) receives the readsb fixture synthesised as CAT021 and
# tracks all 12 aircraft with nothing rejected; skycue-dump --capture then
# decodes the capture file it wrote; and pingsim's usage exits (--help 0, a
# bad --as 2).
# usage: test_pingsim.sh SKYCUED SKYCUE_PINGSIM SKYCUE_DUMP FIXTURE_DIR
set -u
skycued=$1 pingsim=$2 dump=$3 fixtures=$4
sample="$fixtures/readsb-sample.jsonl"
tmp=$(mktemp -d)
bg_pids=()
cleanup() {
  local p i
  for p in "${bg_pids[@]}"; do kill "$p" 2>/dev/null; done
  for p in "${bg_pids[@]}"; do # SIGTERM first; SIGKILL whatever is left after 3 s
    for (( i=0; i<30; i++ )); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$p" 2>/dev/null
  done
  wait 2>/dev/null
  rm -rf "$tmp"
}
trap cleanup EXIT
fail() {
  echo "FAIL: $*"
  [ -s "$tmp/skycued.log" ] && { echo "--- skycued log:"; tail -n 20 "$tmp/skycued.log"; }
  exit 1
}
for f in "$skycued" "$pingsim" "$dump" "$sample"; do [ -e "$f" ] || fail "$f missing"; done

free_port() { # free_port tcp|udp
  python3 -I -c 'import socket,sys
t = socket.SOCK_STREAM if sys.argv[1] == "tcp" else socket.SOCK_DGRAM
s = socket.socket(socket.AF_INET, t); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])' "$1"
}
web_port=$(free_port tcp)
cat_port=$(free_port udp)
mkdir -p "$tmp/rec"
cat > "$tmp/skycue.conf" <<EOF
RECEIVER_LAT=38.9
RECEIVER_LON=-77.0
CUE_TCP_PORT=0
WEB_PORT=$web_port
RECORD_DIR=$tmp/rec
EOF
cat > "$tmp/inputs.json" <<EOF
{"version":1,
 "inputs":[{"id":"sim","type":"pingstation_cat021","transport":"udp","bind":"127.0.0.1","port":$cat_port,"capture":true}]}
EOF
"$skycued" --no-readsb --no-uat --inputs-file "$tmp/inputs.json" --config "$tmp/skycue.conf" \
  >"$tmp/skycued.log" 2>&1 &
bg_pids+=($!)
base="http://127.0.0.1:$web_port"
for (( i=0; i<100; i++ )); do curl -fs -o /dev/null "$base/api/stats" && break; sleep 0.1; done
curl -fs -o /dev/null "$base/api/stats" || fail "web server did not come up"

# The fixture (about 122 s) at 20x.
timeout 60 "$pingsim" --udp "127.0.0.1:$cat_port" --as cat021 --speed 20 "$sample" \
  >"$tmp/pingsim.out" 2>&1
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/pingsim.out"; fail "pingsim exit $rc"; }

# state -> "TRACKS MESSAGES REJECTED" for the sim input
counts() {
  curl -fs "$base/api/state" | python3 -I -c 'import json,sys
s = json.load(sys.stdin)
i = [x for x in s["stats"]["inputs"] if x["id"] == "sim"][0]
print(len(s["tracks"]), i["messages"], i["rejected"])'
}
got=
for (( i=0; i<50; i++ )); do
  got=$(counts)
  [ "${got%% *}" = 12 ] && break
  sleep 0.1
done
read -r tracks messages rejected <<<"$got"
[ "$tracks" = 12 ] || fail "/api/state has '$tracks' tracks, want 12"
[ "$messages" -gt 0 ] || fail "sim input messages $messages"
[ "$rejected" -eq 0 ] || fail "sim input rejected $rejected"

# The capture file (flushed about once a second) decodes.
capture=
for (( i=0; i<50; i++ )); do
  capture=$(ls "$tmp"/rec/capture-sim-*.jsonl 2>/dev/null | head -n 1)
  [ -n "$capture" ] && [ -s "$capture" ] && break
  sleep 0.1
done
[ -n "$capture" ] && [ -s "$capture" ] || { ls -l "$tmp/rec"; fail "no capture file"; }
"$dump" --capture "$capture" >"$tmp/dump.out" 2>"$tmp/dump.err"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/dump.err"; fail "dump --capture exit $rc"; }
grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+Z sim udp [0-9]+ bytes$' "$tmp/dump.out" ||
  { head -n 5 "$tmp/dump.out"; fail "no record header line"; }
n=$(grep -cE '^  CAT021 .*addr=[0-9A-F]{6}' "$tmp/dump.out")
[ "$n" -ge 1 ] || { head -n 5 "$tmp/dump.out"; fail "no CAT021 line with addr="; }
! grep -q '^  BAD' "$tmp/dump.out" || { grep '^  BAD' "$tmp/dump.out" | head; fail "BAD block in the capture"; }

# Usage.
"$pingsim" --help >/dev/null 2>&1
rc=$?
[ "$rc" -eq 0 ] || fail "pingsim --help exit $rc, want 0"
"$pingsim" --udp "127.0.0.1:$cat_port" --as xml "$sample" >"$tmp/xml.out" 2>&1
rc=$?
[ "$rc" -eq 2 ] || { cat "$tmp/xml.out"; fail "pingsim --as xml exit $rc, want 2"; }
grep -q '^skycue-pingsim: ' "$tmp/xml.out" || { cat "$tmp/xml.out"; fail "--as xml message"; }

echo "PASS"
