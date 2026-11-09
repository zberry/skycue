#!/usr/bin/env bash
# The output hub end to end: skycued dials skycue-rawplay (a real Mode S capture
# as Beast frames) through a beast_raw input and serves it on a beast TCP
# output, a beast multicast output and (idle) a uat output; python3 consumers
# check what each one received against the capture file and rawplay's count.
# Phase 2 replays in a loop and adds a consumer that never reads: it must be cut
# at the TCP send cap while the others keep receiving, and removing the output
# with such a consumer backlogged must not disturb anything else.
# usage: test_hub.sh SKYCUED RAWPLAY DATA_DIR
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib/test_common.sh"
skycued=$1 rawplay=$2 data_dir=$3
csv="$data_dir/raw/modes-120s.csv.gz"
speed1=${SKYCUE_HUB_SPEED1:-20}   # phase 1: 120 s of capture in about 6 s
speed2=${SKYCUE_HUB_SPEED2:-40}   # phase 2: the 1 MiB cap of a stalled consumer trips in about 3 s
test_init hub
fail_context() {
  for f in skycued.log play1.log play2.log; do
    [ -s "$tmp/$f" ] && { echo "--- $f:"; tail -n 15 "$tmp/$f"; }
  done
}
[ -e "$csv" ] || { echo "SKIP: $csv not fetched: run scripts/test-data/fetch-test-captures.sh"; exit 77; }
command -v python3 >/dev/null || { echo "SKIP: python3 missing"; exit 77; }
command -v curl >/dev/null || { echo "SKIP: curl missing"; exit 77; }
for f in "$skycued" "$rawplay"; do [ -e "$f" ] || fail "$f missing"; done

# consumer.py MODE ARGS...  (output file written unbuffered; ends on SIGTERM or EOF)
#   tcp PORT OUT [stall]   connect to 127.0.0.1:PORT; "stall" = 4 KiB receive buffer, never read
#   udp GROUP PORT OUT     join GROUP on 127.0.0.1; each datagram stored as a 4-byte length + body
cat > "$tmp/consumer.py" <<'EOF'
import socket, struct, sys, time, os
mode = sys.argv[1]
if mode == "tcp":
    port, out = int(sys.argv[2]), sys.argv[3]
    stall = len(sys.argv) > 4
    s = socket.socket()
    if stall:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.connect(("127.0.0.1", port))
    f = open(out, "wb", buffering=0)
    if stall:
        time.sleep(3600)
    while True:
        b = s.recv(65536)
        if not b:
            break
        f.write(b)
else:
    group, port, out = sys.argv[2], int(sys.argv[3]), sys.argv[4]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    s.bind(("", port))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 socket.inet_aton(group) + socket.inet_aton("127.0.0.1"))
    f = open(out, "wb", buffering=0)
    open(out + ".ready", "w").close()
    while True:
        d = s.recv(65535)
        f.write(struct.pack(">I", len(d)) + d)
EOF
# beast.py: split Beast bytes into frames (unescaped payload kept escaped as on the wire).
cat > "$tmp/check.py" <<'EOF'
import struct, sys
def frames(buf):
    out, i, n = [], 0, len(buf)
    while i < n:
        if buf[i] != 0x1a or i + 1 >= n or buf[i+1] not in b"123":
            raise SystemExit("not a Beast stream at offset %d" % i)
        want = 6 + 1 + {ord("1"): 2, ord("2"): 7, ord("3"): 14}[buf[i+1]]
        j, got = i + 2, 0
        while got < want:
            if j >= n: raise SystemExit("truncated frame at offset %d" % i)
            if buf[j] == 0x1a:
                if j + 1 >= n or buf[j+1] != 0x1a: raise SystemExit("bad escape at %d" % j)
                j += 1
            j += 1; got += 1
        out.append(bytes(buf[i:j])); i = j
    return out
cmd = sys.argv[1]
rd = lambda p: open(p, "rb").read()
if cmd == "count":
    print(len(frames(rd(sys.argv[2]))))
elif cmd == "mc":      # mc DGRAMFILE TCPFILE: datagrams, each one whole frame, concatenate to TCPFILE
    d, pos, grams = rd(sys.argv[2]), 0, []
    while pos < len(d):
        (l,) = struct.unpack(">I", d[pos:pos+4]); grams.append(d[pos+4:pos+4+l]); pos += 4 + l
    for g in grams:
        if len(frames(g)) != 1: raise SystemExit("a datagram is not one whole frame")
    if b"".join(grams) != rd(sys.argv[3]): raise SystemExit("datagrams differ from the TCP stream")
    print(len(grams))
elif cmd == "field":   # field JSON ID KEY  (KEY is a state key)
    import json
    o = [x for x in json.loads(rd(sys.argv[2]))["outputs"] if x["id"] == sys.argv[3]]
    print(json.dumps(o[0]["state"][sys.argv[4]]) if o else "none")
EOF

api_port=$(free_port); cue_port=$(free_port); raw_port=$(free_port)
tcp_port=$(free_port); uat_port=$(free_port); mc_port=$(free_port udp)
group="239.255.$((RANDOM % 200 + 20)).$((RANDOM % 200 + 20))"
base="http://127.0.0.1:$api_port"
mkdir -p "$tmp/rec"
cat > "$tmp/skycue.toml" <<EOF
[receiver]
lat = 38.9
lon = -77.0
[cues]
tcp_port = $cue_port
[web]
port = $api_port
[recording]
dir = "$tmp/rec"
EOF
cat > "$tmp/hub.inputs.json" <<EOF
{"version":2,
 "inputs":[
  {"id":"lab","name":"Lab","type":"beast_raw","transport":"tcp_connect","host":"127.0.0.1","port":$raw_port,"capture":true}
 ],
 "outputs":[
  {"id":"b-tcp","name":"Beast TCP","kind":"beast","transport":"tcp_listen","bind":"127.0.0.1","port":$tcp_port},
  {"id":"b-mc","name":"Beast multicast","kind":"beast","transport":"udp","group":"$group","port":$mc_port,"interface":"127.0.0.1"},
  {"id":"u-tcp","name":"UAT TCP","kind":"uat","transport":"tcp_listen","bind":"127.0.0.1","port":$uat_port}
 ],
 "builtin":{}}
EOF

"$skycued" --no-readsb --no-uat --config "$tmp/skycue.toml" --inputs-file "$tmp/hub.inputs.json" \
  >"$tmp/skycued.log" 2>&1 &
sk=$!; bg_pids+=("$sk")
api_up() { curl -fs -o "$tmp/outputs.json" "$base/api/outputs"; }
wait_for 15 api_up || fail "skycued API did not come up"
outputs_up() { api_up && [ "$(python3 -I "$tmp/check.py" field "$tmp/outputs.json" b-tcp up)" = true ] \
  && [ "$(python3 -I "$tmp/check.py" field "$tmp/outputs.json" u-tcp up)" = true ]; }
wait_for 15 outputs_up || fail "outputs b-tcp and u-tcp not up"
field() { api_up && python3 -I "$tmp/check.py" field "$tmp/outputs.json" "$1" "$2"; }

# ---- phase 1: finite replay, four consumers -----------------------------------
bg python3 -I "$tmp/consumer.py" tcp "$tcp_port" "$tmp/A.bin"
bg python3 -I "$tmp/consumer.py" tcp "$tcp_port" "$tmp/B.bin"
bg python3 -I "$tmp/consumer.py" udp "$group" "$mc_port" "$tmp/M.dg"
bg python3 -I "$tmp/consumer.py" tcp "$uat_port" "$tmp/U.bin"
two_peers() { [ "$(field b-tcp peers)" = 2 ] && [ "$(field u-tcp peers)" = 1 ]; }
wait_for 10 two_peers || fail "consumers not connected (b-tcp peers $(field b-tcp peers))"
wait_for 10 test -e "$tmp/M.dg.ready" || fail "multicast consumer not ready"

"$rawplay" --1090-listen "$raw_port" --wait-clients 1 --speed "$speed1" "$csv" >"$tmp/play1.log" 2>&1 &
play=$!; bg_pids+=("$play")
t0=$(date +%s.%N)
wait_for 90 bash -c "grep -q 'done:' '$tmp/play1.log'" || fail "rawplay did not finish"
t1=$(date +%s.%N)
sent=$(sed -n 's/.*done: \([0-9]*\) frames sent.*/\1/p' "$tmp/play1.log" | head -n 1)
[ -n "$sent" ] && [ "$sent" -gt 100000 ] || fail "rawplay's done line: '$(grep done: "$tmp/play1.log")'"
# the stream is complete when A holds the counter's half of the bytes and stops growing
settled() {
  local b a; b=$(field b-tcp bytes); a=$(stat -c %s "$tmp/A.bin")
  [ "$b" -gt 0 ] && [ $((b % 2)) = 0 ] && [ "$a" = $((b / 2)) ] && [ "$(stat -c %s "$tmp/B.bin")" = "$a" ]
}
wait_for 20 settled || fail "consumers did not settle: A $(stat -c %s "$tmp/A.bin"), B $(stat -c %s "$tmp/B.bin"), bytes $(field b-tcp bytes)"
alen=$(stat -c %s "$tmp/A.bin")

cmp -s "$tmp/A.bin" "$tmp/B.bin" || fail "hub_consumers_identical"
echo "ok hub_consumers_identical ($alen bytes each)"

dg=$(python3 -I "$tmp/check.py" mc "$tmp/M.dg" "$tmp/A.bin") || fail "hub_multicast_equals_tcp"
echo "ok hub_multicast_equals_tcp ($dg datagrams)"

cap=$(find "$tmp/rec" -name 'raw-lab-*.bin' | head -n 1)
[ -n "$cap" ] || fail "no raw-lab-*.bin under RECORD_DIR ($(find "$tmp/rec" -type f | head -n 5 | tr '\n' ' '))"
cap_ok() { cmp -s "$cap" "$tmp/A.bin"; }
wait_for 5 cap_ok || fail "hub_capture_equals_consumers ($(stat -c %s "$cap") vs $alen bytes)"
echo "ok hub_capture_equals_consumers"

frames=$(python3 -I "$tmp/check.py" count "$tmp/A.bin") || fail "A is not a clean Beast stream"
[ "$frames" = "$sent" ] || fail "hub_frame_count_equals_rawplay: $frames frames, rawplay sent $sent"
echo "ok hub_frame_count_equals_rawplay ($frames frames, speed $speed1, $(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", b-a}') s)"

[ "$(field u-tcp up)" = true ] && [ "$(field u-tcp bytes)" = 0 ] || fail "hub_uat_output_empty_and_up: up $(field u-tcp up) bytes $(field u-tcp bytes)"
[ "$(stat -c %s "$tmp/U.bin")" = 0 ] || fail "the uat consumer received bytes"
echo "ok hub_uat_output_empty_and_up"

[ "$(field b-tcp messages)" = $((2 * sent)) ] || fail "hub_counters_match: messages $(field b-tcp messages), want $((2 * sent))"
[ "$(field b-tcp bytes)" = $((2 * alen)) ] || fail "hub_counters_match: bytes $(field b-tcp bytes), want $((2 * alen))"
echo "ok hub_counters_match"

# --check-inputs dials every tcp_connect input once: the same file with the
# beast_raw input pointed at the running daemon's TCP output, which accepts.
sed "s/\"port\":$raw_port/\"port\":$tcp_port/" "$tmp/hub.inputs.json" > "$tmp/check.inputs.json"
"$skycued" --no-readsb --no-uat --config "$tmp/skycue.toml" --inputs-file "$tmp/check.inputs.json" \
  --check-inputs >"$tmp/check.out" 2>&1 || { cat "$tmp/check.out"; fail "hub_check_inputs_still_passes: exit $?"; }
grep -q "lab 127.0.0.1:$tcp_port: connected" "$tmp/check.out" || { cat "$tmp/check.out"; fail "hub_check_inputs_still_passes: no connected line"; }
echo "ok hub_check_inputs_still_passes"

# ---- phase 2: looped replay, a consumer that never reads ----------------------
kill "${bg_pids[@]:1}" 2>/dev/null; wait "${bg_pids[@]:1}" 2>/dev/null
bg_pids=("$sk")
rm -f "$tmp/M.dg.ready"
bg python3 -I "$tmp/consumer.py" tcp "$tcp_port" "$tmp/A2.bin"
bg python3 -I "$tmp/consumer.py" udp "$group" "$mc_port" "$tmp/M2.dg"
wait_for 10 test -e "$tmp/M2.dg.ready" || fail "phase 2 multicast consumer not ready"
python3 -I "$tmp/consumer.py" tcp "$tcp_port" "$tmp/C.bin" stall & cpid=$!; bg_pids+=("$cpid")
b_tcp_two_peers() { [ "$(field b-tcp peers)" = 2 ]; }
wait_for 10 b_tcp_two_peers || fail "phase 2 consumers not connected (peers $(field b-tcp peers))"
d0=$(field b-tcp dropped)
bg "$rawplay" --1090-listen "$raw_port" --wait-clients 1 --speed "$speed2" --loop "$csv" >"$tmp/play2.log" 2>&1
c0=$(date +%s.%N)
cut() { [ "$(field b-tcp dropped)" -gt "$d0" ] && [ "$(field b-tcp peers)" = 1 ]; }
wait_for 60 cut || fail "hub_stalled_consumer_cut_others_continue: not cut (dropped $(field b-tcp dropped), peers $(field b-tcp peers))"
cut_s=$(awk -v a="$c0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}')
a1=$(stat -c %s "$tmp/A2.bin"); m1=$(stat -c %s "$tmp/M2.dg")
slow_sleep 2
a2=$(stat -c %s "$tmp/A2.bin"); m2=$(stat -c %s "$tmp/M2.dg")
[ "$a2" -gt "$a1" ] || fail "hub_stalled_consumer_cut_others_continue: A2 stopped at $a1"
[ "$m2" -gt "$m1" ] || fail "hub_stalled_consumer_cut_others_continue: multicast stopped at $m1"
echo "ok hub_stalled_consumer_cut_others_continue (cut after ${cut_s} s at speed $speed2, A2 +$((a2 - a1)) bytes in 2 s)"

# a second stalled consumer, backlogged when the output is removed
bg python3 -I "$tmp/consumer.py" tcp "$tcp_port" "$tmp/C2.bin" stall
wait_for 10 b_tcp_two_peers || fail "second stalled consumer not connected"
slow_sleep 2
code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$base/api/outputs/b-tcp")
[ "$code" = 204 ] || fail "hub_remove_output_with_backlog: DELETE returned $code"
[ "$(field u-tcp up)" = true ] || fail "hub_remove_output_with_backlog: u-tcp not up after the delete"
python3 -I -c 'import socket,sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=3); s.close()' "$cue_port" ||
  fail "hub_remove_output_with_backlog: the cue port does not answer"
rebind() { python3 -I -c 'import socket,sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(1)' "$tcp_port" 2>/dev/null; }
wait_for 3 rebind || fail "hub_remove_output_with_backlog: port $tcp_port not rebindable within 3 s"
kill -0 "$sk" 2>/dev/null || fail "skycued died"
echo "ok hub_remove_output_with_backlog"
echo "PASS hub: $frames frames, $alen bytes, $dg datagrams, cut after ${cut_s} s"
