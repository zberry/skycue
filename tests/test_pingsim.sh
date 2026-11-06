#!/usr/bin/env bash
# skycue-pingsim end to end, against one skycued with only pingStation inputs:
#  (a) the readsb fixture synthesised as CAT021 into a UDP input with capture
#      on: all 12 aircraft tracked, >= 95% of the sent datagrams counted,
#      nothing rejected; skycue-dump --capture decodes the capture file;
#  (b) the fixture as pingStation JSON with a status message every 2 s into a
#      pingstation_json input: >= 95% counted, nothing rejected, and the
#      status shows as device_status on /api/inputs/sim-json/status;
#  (c) that capture file replayed with --capture into a fresh input: the same
#      message count as the file has records;
#  (d) --tcp-listen (on 127.0.0.1 by default) to a tcp_connect input added
#      over the API: every block counted;
#  (e) usage: --help exits 0, a bad --as exits 2, --loop over a file with
#      nothing to play exits 1.
# usage: test_pingsim.sh SKYCUED SKYCUE_PINGSIM SKYCUE_DUMP FIXTURE_DIR
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib/test_common.sh"
skycued=$1 pingsim=$2 dump=$3 fixtures=$4
sample="$fixtures/readsb-sample.jsonl"
tmp=$(mktemp -d)
bg_pids=()
cleanup() {
  local p i
  for p in "${bg_pids[@]}"; do kill "$p" 2>/dev/null; done
  for p in "${bg_pids[@]}"; do # SIGTERM first; SIGKILL whatever is left after 3 s
    for (( i=0; i<30*slowf; i++ )); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
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
json_port=$(free_port udp)
replay_port=$(free_port udp)
listen_port=$(free_port tcp)
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
 "inputs":[
  {"id":"sim","type":"pingstation_cat021","transport":"udp","bind":"127.0.0.1","port":$cat_port,"capture":true},
  {"id":"sim-json","type":"pingstation_json","bind":"127.0.0.1","port":$json_port},
  {"id":"replayed","type":"pingstation_cat021","transport":"udp","bind":"127.0.0.1","port":$replay_port}
 ]}
EOF
"$skycued" --no-readsb --no-uat --inputs-file "$tmp/inputs.json" --config "$tmp/skycue.conf" \
  >"$tmp/skycued.log" 2>&1 &
bg_pids+=($!)
base="http://127.0.0.1:$web_port"
for (( i=0; i<100*slowf; i++ )); do curl -fs -o /dev/null "$base/api/stats" && break; sleep 0.1; done
curl -fs -o /dev/null "$base/api/stats" || fail "web server did not come up"

# counts ID -> "TRACKS MESSAGES REJECTED" (messages and rejected 0 for an input not listed yet)
counts() {
  curl -fs "$base/api/state" | python3 -I -c 'import json,sys
s = json.load(sys.stdin)
i = [x for x in s["stats"]["inputs"] if x["id"] == sys.argv[1]] or [{"messages": 0, "rejected": 0}]
print(len(s["tracks"]), i[0]["messages"], i[0]["rejected"])' "$1"
}
# sent LOG -> the datagrams pingsim's summary line reports: records plus status messages
sent() {
  sed -nE 's/.* done: ([0-9]+) records sent, [0-9]+ lines skipped, ([0-9]+) status messages.*/\1 \2/p' "$1" |
    awk '{ print $1 + $2 }'
}
# wait_messages ID WANT: sets tracks/messages/rejected, polling until the
# input has counted WANT messages. It gives up when the count has not grown for
# 3 s, or after 20 s in all (both scaled by SKYCUE_TEST_SLOW), so an emulated or
# loaded machine that is still working through the datagrams is waited for.
wait_messages() {
  local i stall=0 last=-1
  for (( i=0; i<200*slowf; i++ )); do
    read -r tracks messages rejected <<<"$(counts "$1")"
    [ "$messages" -ge "$2" ] && return
    if [ "$messages" -gt "$last" ]; then stall=0; last=$messages; else stall=$(( stall + 1 )); fi
    [ "$stall" -ge $(( 30 * slowf )) ] && return
    sleep 0.1
  done
}
# at_least_95 GOT SENT
at_least_95() { [ $(( $1 * 100 )) -ge $(( $2 * 95 )) ]; }

# ---- (a) CAT021 over UDP, capture on -------------------------------------------
# The fixture (about 122 s) at 20x.
timeout $((60*slowf)) "$pingsim" --udp "127.0.0.1:$cat_port" --as cat021 --speed 20 "$sample" >"$tmp/a.log" 2>&1
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/a.log"; fail "(a) pingsim exit $rc"; }
a_sent=$(sent "$tmp/a.log")
[ "${a_sent:-0}" -eq 1394 ] || { cat "$tmp/a.log"; fail "(a) pingsim reports '$a_sent' sent, want 1394"; }
wait_messages sim "$a_sent"
for (( i=0; i<50*slowf; i++ )); do
  [ "$tracks" = 12 ] && break
  sleep 0.1
  read -r tracks messages rejected <<<"$(counts sim)"
done
[ "$tracks" = 12 ] || fail "(a) /api/state has '$tracks' tracks, want 12"
at_least_95 "$messages" "$a_sent" || fail "(a) sim counted $messages of $a_sent datagrams"
[ "$rejected" -eq 0 ] || fail "(a) sim rejected $rejected"
a_messages=$messages

# The capture file (flushed about once a second) holds every datagram counted.
capture= lines=0
for (( i=0; i<50*slowf; i++ )); do
  capture=$(ls "$tmp"/rec/capture-sim-*.jsonl 2>/dev/null | head -n 1)
  [ -n "$capture" ] && lines=$(wc -l <"$capture") && [ "$lines" -eq "$a_messages" ] && break
  sleep 0.1
done
[ -n "$capture" ] || { ls -l "$tmp/rec"; fail "(a) no capture file"; }
[ "$lines" -eq "$a_messages" ] || fail "(a) capture has $lines records, the input counted $a_messages"
"$dump" --capture "$capture" >"$tmp/dump.out" 2>"$tmp/dump.err"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/dump.err"; fail "(a) dump --capture exit $rc"; }
grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+Z sim udp [0-9]+ bytes$' "$tmp/dump.out" ||
  { head -n 5 "$tmp/dump.out"; fail "(a) no record header line"; }
n=$(grep -cE '^  CAT021 .*addr=[0-9A-F]{6}' "$tmp/dump.out")
[ "$n" -ge 1 ] || { head -n 5 "$tmp/dump.out"; fail "(a) no CAT021 line with addr="; }
! grep -q '^  BAD' "$tmp/dump.out" || { grep '^  BAD' "$tmp/dump.out" | head; fail "(a) BAD block in the capture"; }

# ---- (b) JSON with status messages ------------------------------------------------
timeout $((60*slowf)) "$pingsim" --udp "127.0.0.1:$json_port" --as json --speed 20 \
  --status-interval 2 --status-lat 38.85 --status-lon -77.05 "$sample" >"$tmp/b.log" 2>&1
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/b.log"; fail "(b) pingsim exit $rc"; }
b_sent=$(sent "$tmp/b.log")
[ "${b_sent:-0}" -gt 1394 ] || { cat "$tmp/b.log"; fail "(b) pingsim reports '$b_sent' sent, want records and statuses"; }
wait_messages sim-json "$b_sent"
at_least_95 "$messages" "$b_sent" || fail "(b) sim-json counted $messages of $b_sent datagrams"
[ "$rejected" -eq 0 ] || fail "(b) sim-json rejected $rejected"
curl -fs "$base/api/inputs/sim-json/status" >"$tmp/status.json" || fail "(b) GET status"
python3 -I -c 'import json,sys
d = json.load(open(sys.argv[1]))["device_status"]
assert d["guid"] == "0000000000000001", d
assert d["version"] == "1.0.0", d
assert d["gps_status"] == 3 and d["receiver_status"] == 0, d
assert abs(d["lat"] - 38.85) < 1e-9 and abs(d["lon"] + 77.05) < 1e-9, d' "$tmp/status.json" ||
  { cat "$tmp/status.json"; fail "(b) device_status"; }

# ---- (c) the capture replayed into a fresh input -----------------------------------
# The capture's timestamps were recorded at 20x in (a), so they already carry
# the compressed timing; --speed 20 again would fire every datagram at once and
# overrun the socket buffer of a slow (emulated) consumer. Replay at 1x.
timeout $((60*slowf)) "$pingsim" --udp "127.0.0.1:$replay_port" --capture "$capture" --speed 1 >"$tmp/c.log" 2>&1
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/c.log"; fail "(c) pingsim exit $rc"; }
[ "$(sent "$tmp/c.log")" = "$lines" ] || { cat "$tmp/c.log"; fail "(c) pingsim did not send $lines records"; }
wait_messages replayed "$lines"
# UDP on loopback can still drop a datagram when the machine is loaded (a
# parallel ctest), as (a) and (b) allow for; pingsim sending every record is
# checked exactly above.
at_least_95 "$messages" "$lines" || fail "(c) replayed counted $messages, the capture has $lines"
[ "$rejected" -eq 0 ] || fail "(c) replayed rejected $rejected"

# ---- (d) --tcp-listen, loopback by default, to a tcp_connect input ---------------
timeout $((60*slowf)) "$pingsim" --tcp-listen "$listen_port" --as cat021 --speed 0 "$sample" >"$tmp/d.log" 2>&1 &
d_pid=$!
bg_pids+=($d_pid)
listening=
for (( i=0; i<50*slowf; i++ )); do # the local address of the LISTEN socket, from /proc/net/tcp{,6}
  listening=$(python3 -I -c 'import sys
port = "%04X" % int(sys.argv[1])
for f in ("/proc/net/tcp", "/proc/net/tcp6"):
    try:
        lines = open(f).readlines()[1:]
    except OSError:
        continue
    for line in lines:
        local, state = line.split()[1], line.split()[3]
        if state == "0A" and local.endswith(":" + port):
            print(local.split(":")[0])' "$listen_port")
  [ -n "$listening" ] && break
  sleep 0.1
done
[ "$listening" = 0100007F ] || fail "(d) pingsim listens on '$listening', want 127.0.0.1 only (0100007F)"
code=$(curl -s -o "$tmp/post.out" -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
  -d "{\"id\":\"tcp-sim\",\"type\":\"pingstation_cat021\",\"transport\":\"tcp_connect\",\"host\":\"127.0.0.1\",\"port\":$listen_port}" \
  "$base/api/inputs")
[ "$code" = 201 ] || { cat "$tmp/post.out"; fail "(d) POST /api/inputs: $code"; }
wait "$d_pid"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/d.log"; fail "(d) pingsim exit $rc"; }
wait_messages tcp-sim 1394
[ "$messages" -eq 1394 ] || fail "(d) tcp-sim counted $messages blocks, want 1394"
[ "$rejected" -eq 0 ] || fail "(d) tcp-sim rejected $rejected"

# ---- (e) usage --------------------------------------------------------------------
"$pingsim" --help >/dev/null 2>&1
rc=$?
[ "$rc" -eq 0 ] || fail "(e) pingsim --help exit $rc, want 0"
"$pingsim" --udp "127.0.0.1:$cat_port" --as xml "$sample" >"$tmp/xml.out" 2>&1
rc=$?
[ "$rc" -eq 2 ] || { cat "$tmp/xml.out"; fail "(e) pingsim --as xml exit $rc, want 2"; }
grep -q '^skycue-pingsim: ' "$tmp/xml.out" || { cat "$tmp/xml.out"; fail "(e) --as xml message"; }
echo '{"not":"a recording"}' >"$tmp/nothing.jsonl"
timeout $((10*slowf)) "$pingsim" --udp "127.0.0.1:$cat_port" --as cat021 --speed 0 --loop "$tmp/nothing.jsonl" \
  >"$tmp/loop.out" 2>&1
rc=$?
[ "$rc" -eq 1 ] || { cat "$tmp/loop.out"; fail "(e) pingsim --loop over nothing exit $rc, want 1"; }

echo "PASS"
