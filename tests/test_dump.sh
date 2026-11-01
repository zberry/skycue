#!/usr/bin/env bash
# skycue-dump against a real skycued replaying the readsb JSON fixture (text
# and --json over TCP, text over UDP), exit 3 when nothing arrives before
# --timeout, and against a python peer that sends hand-made frames: heartbeats,
# a malformed TrackUpdate (BAD), a drop for an unknown track, then a hang-up
# (CONN down, LOCAL-DROP), with only frame lines counting towards --count;
# and --tcp-listen with a second skycued that has no listener (CUE_TCP_PORT=0)
# and connects out to the dump (CUE_TCP_CONNECT).
# usage: test_dump.sh SKYCUED SKYCUE_DUMP FIXTURE_DIR
set -u
skycued=$1 dump=$2 fixtures=$3
replay="$fixtures/readsb-sample.jsonl"
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
for f in "$skycued" "$dump" "$replay"; do [ -e "$f" ] || fail "$f missing"; done

free_port() { # free_port tcp|udp
  python3 -I -c 'import socket,sys
t = socket.SOCK_STREAM if sys.argv[1] == "tcp" else socket.SOCK_DGRAM
s = socket.socket(socket.AF_INET, t); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])' "$1"
}
tcp_port=$(free_port tcp)
udp_port=$(free_port udp)
# KEYFRAME_INTERVAL_S=2: UDP targets get a Hello before every keyframe burst.
cat > "$tmp/skycue.conf" <<EOF
RECEIVER_LAT=38.9
RECEIVER_LON=-77.0
CUE_TCP_PORT=$tcp_port
CUE_UDP_TARGETS=127.0.0.1:$udp_port
KEYFRAME_INTERVAL_S=2
EOF
"$skycued" --replay "$replay" --replay-speed 50 --replay-loop --config "$tmp/skycue.conf" \
  >"$tmp/skycued.log" 2>&1 &
bg_pids+=($!)

ts='^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z '
frame_re="${ts}(HELLO|UPDATE|DROP|HB|BAD)( |$)"
event_re="${ts}(CONN (up|down)|LOCAL-DROP [0-9A-F]{6} [a-z_]+)$"
update_re="${ts}UPDATE [0-9A-F]{6} [a-z_]+ [a-z0-9_]+ -?[0-9]+\.[0-9]{7} -?[0-9]+\.[0-9]{7}( |$)"
frames() { grep -E "$frame_re" "$1"; }
check_lines() { # check_lines FILE LABEL: every line is a frame or an event line
  local bad
  bad=$(grep -cvE "($frame_re)|($event_re)" "$1")
  [ "$bad" -eq 0 ] || { cat "$1"; fail "$2 $bad malformed lines"; }
}

# ---- (a) text over TCP: --count frame lines, HELLO first, some UPDATEs --------
timeout 30 "$dump" --tcp "127.0.0.1:$tcp_port" --count 20 --timeout 15 >"$tmp/a.out" 2>"$tmp/a.err"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/a.err"; fail "(a) exit $rc"; }
check_lines "$tmp/a.out" "(a)"
n=$(frames "$tmp/a.out" | wc -l)
[ "$n" -eq 20 ] || { cat "$tmp/a.out"; fail "(a) $n frame lines, want 20"; }
head -n 1 "$tmp/a.out" | grep -qE "${ts}CONN up$" || fail "(a) first line is not CONN up"
frames "$tmp/a.out" | head -n 1 | grep -qE "${ts}HELLO skycued kf=2000ms hb=[0-9]+ms$" ||
  fail "(a) first frame is not HELLO: $(frames "$tmp/a.out" | head -n 1)"
grep -qE "$update_re" "$tmp/a.out" || { cat "$tmp/a.out"; fail "(a) no UPDATE line"; }
! grep -q ' LOCAL-DROP ' "$tmp/a.out" || { cat "$tmp/a.out"; fail "(a) LOCAL-DROP on a live connection"; }

# ---- (b) --json over TCP: one JSON object per line ---------------------------
timeout 30 "$dump" --tcp "127.0.0.1:$tcp_port" --json --count 10 --timeout 15 >"$tmp/b.out" 2>"$tmp/b.err"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/b.err"; fail "(b) exit $rc"; }
python3 -I -c 'import json,sys
objs = [json.loads(l) for l in sys.stdin]
for o in objs:
    assert isinstance(o["time"], str), o
    assert o["frame"] in ("hello","update","drop","heartbeat","bad","conn","local_drop"), o
    assert (o.get("counted", True) is False) == (o["frame"] in ("conn","local_drop")), o
counted = [o for o in objs if o.get("counted", True)]
assert len(counted) == 10, len(counted)
assert counted[0]["frame"] == "hello", counted[0]
assert any(o["frame"] == "update" for o in counted)' < "$tmp/b.out" || { cat "$tmp/b.out"; fail "(b) JSON check"; }

# ---- (c) text over UDP: --count, then a HELLO and UPDATEs within 5 s -----------
timeout 30 "$dump" --udp "127.0.0.1:$udp_port" --count 5 --timeout 15 >"$tmp/c.out" 2>"$tmp/c.err"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/c.err"; fail "(c) exit $rc"; }
check_lines "$tmp/c.out" "(c)"
n=$(frames "$tmp/c.out" | wc -l)
[ "$n" -eq 5 ] || { cat "$tmp/c.out"; fail "(c) $n frame lines, want 5"; }
timeout 30 "$dump" --udp "127.0.0.1:$udp_port" --timeout 5 >"$tmp/c2.out" 2>"$tmp/c2.err"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/c2.err"; fail "(c2) exit $rc"; }
check_lines "$tmp/c2.out" "(c2)"
grep -qE "${ts}HELLO skycued kf=2000ms hb=[0-9]+ms$" "$tmp/c2.out" || { cat "$tmp/c2.out"; fail "(c2) no HELLO"; }
grep -qE "$update_re" "$tmp/c2.out" || { cat "$tmp/c2.out"; fail "(c2) no UPDATE"; }

# ---- (d) nothing to receive: exit 3 after --timeout, no output ----------------
dead_port=$(free_port tcp)
timeout 30 "$dump" --tcp "127.0.0.1:$dead_port" --timeout 1 >"$tmp/d.out" 2>"$tmp/d.err"
rc=$?
[ "$rc" -eq 3 ] || { cat "$tmp/d.err"; fail "(d) exit $rc, want 3"; }
[ ! -s "$tmp/d.out" ] || fail "(d) unexpected output"

# ---- (e) hand-made frames from a python peer ------------------------------------
# One peer per run: it accepts one connection, stops listening (so the dump's
# reconnects are refused), sends the frames and hangs up.
cat > "$tmp/peer.py" <<'EOF'
import socket, struct, sys, os, time
def frame(t, p): return b"SKYC" + struct.pack("<BBH", 1, t, len(p)) + p
def update(addr, alt):  # keyframe, adsb_1090 icao, 38.9 / -77.0, alt_baro only
    return frame(2, struct.pack("<IBBBBQiiH", addr, 0, 0, 1, 0, 1793545200000, 389000000, -770000000, 1)
                 + struct.pack("<i", alt))
def drop(addr, reason): return frame(3, struct.pack("<IBB", addr, 0, reason))
frames = [
    frame(1, struct.pack("<II", 10000, 1000) + b"\x04test"),
    frame(4, struct.pack("<Q", 1)),
    frame(2, b"\x01\x02\x03"),   # truncated TrackUpdate -> BAD update 3
    drop(0x00ABCD, 0),           # unknown to the client, still a wire frame
    update(0xABC123, 35000),
    drop(0xABC123, 1),           # known: one DROP, no LOCAL-DROP
    update(0xABC124, 12000),     # left in the table: source_lost on hang-up
    frame(4, struct.pack("<Q", 2)),
]
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(1); s.settimeout(20)
open(sys.argv[1] + ".tmp", "w").write(str(s.getsockname()[1])); os.rename(sys.argv[1] + ".tmp", sys.argv[1])
c, _ = s.accept()
s.close()
try:
    c.sendall(b"".join(frames))
    time.sleep(0.5)
except OSError:
    pass
c.close()
EOF
start_peer() { # sets $peer to HOST:PORT of a fresh peer
  rm -f "$tmp/peerport"
  python3 -I "$tmp/peer.py" "$tmp/peerport" &
  bg_pids+=($!)
  local i
  for (( i=0; i<50; i++ )); do [ -s "$tmp/peerport" ] && break; sleep 0.1; done
  [ -s "$tmp/peerport" ] || fail "(e) python peer did not start"
  peer="127.0.0.1:$(cat "$tmp/peerport")"
}
strip() { sed -E 's/^[^ ]+ //' "$1" | tr '\n' '|'; }

# (e1) the whole session: frames, then CONN down and the client's own drop.
start_peer
timeout 30 "$dump" --tcp "$peer" --timeout 2 >"$tmp/e1.out" 2>"$tmp/e1.err"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/e1.err"; fail "(e1) exit $rc"; }
want="CONN up|HELLO test kf=10000ms hb=1000ms|HB|BAD update 3|DROP 00ABCD timeout|"
want+="UPDATE ABC123 icao adsb_1090 38.9000000 -77.0000000 alt_baro=35000 kf|DROP ABC123 filtered|"
want+="UPDATE ABC124 icao adsb_1090 38.9000000 -77.0000000 alt_baro=12000 kf|HB|CONN down|LOCAL-DROP ABC124 source_lost|"
got=$(strip "$tmp/e1.out")
[ "$got" = "$want" ] || fail "(e1) got  '$got'
want '$want'"

# (e2) --count 4 counts HB and BAD but not CONN.
start_peer
timeout 30 "$dump" --tcp "$peer" --count 4 --timeout 10 >"$tmp/e2.out" 2>"$tmp/e2.err"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/e2.err"; fail "(e2) exit $rc"; }
got=$(strip "$tmp/e2.out")
[ "$got" = "CONN up|HELLO test kf=10000ms hb=1000ms|HB|BAD update 3|DROP 00ABCD timeout|" ] ||
  fail "(e2) got '$got'"

# (e3) the same session as JSON.
start_peer
timeout 30 "$dump" --tcp "$peer" --json --timeout 2 >"$tmp/e3.out" 2>"$tmp/e3.err"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/e3.err"; fail "(e3) exit $rc"; }
python3 -I -c 'import json,sys
o = [json.loads(l) for l in sys.stdin]
assert [x["frame"] for x in o] == ["conn","hello","heartbeat","bad","drop","update","drop","update",
                                  "heartbeat","conn","local_drop"], [x["frame"] for x in o]
assert o[0]["connected"] is True and o[0]["counted"] is False
assert o[3]["type"] == "update" and o[3]["length"] == 3
assert o[4]["hex"] == "00ABCD" and o[4]["reason"] == "timeout"
assert o[5]["alt_baro_ft"] == 35000 and o[5]["keyframe"] is True
assert o[9]["connected"] is False and o[9]["counted"] is False
assert o[10] == {**o[10], "hex": "ABC124", "reason": "source_lost", "counted": False}
assert all("counted" not in x for x in o[1:9])' < "$tmp/e3.out" || { cat "$tmp/e3.out"; fail "(e3) JSON check"; }

# ---- (f) --tcp-listen: a skycued with no listener connects out to the dump -----
listen_port=$(free_port tcp)
cat > "$tmp/skycue-out.conf" <<EOF
RECEIVER_LAT=38.9
RECEIVER_LON=-77.0
CUE_TCP_PORT=0
CUE_TCP_CONNECT=127.0.0.1:$listen_port
KEYFRAME_INTERVAL_S=2
EOF
timeout 30 "$dump" --tcp-listen "127.0.0.1:$listen_port" --count 10 --timeout 15 >"$tmp/f.out" 2>"$tmp/f.err" &
dump_pid=$!
"$skycued" --replay "$replay" --replay-speed 50 --replay-loop --config "$tmp/skycue-out.conf" \
  >"$tmp/skycued-out.log" 2>&1 &
bg_pids+=($!)
wait "$dump_pid"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/f.err"; tail -n 20 "$tmp/skycued-out.log"; fail "(f) exit $rc"; }
check_lines "$tmp/f.out" "(f)"
n=$(frames "$tmp/f.out" | wc -l)
[ "$n" -eq 10 ] || { cat "$tmp/f.out"; fail "(f) $n frame lines, want 10"; }
head -n 1 "$tmp/f.out" | grep -qE "${ts}CONN up$" || fail "(f) first line is not CONN up"
frames "$tmp/f.out" | head -n 1 | grep -qE "${ts}HELLO skycued kf=2000ms hb=[0-9]+ms$" ||
  fail "(f) first frame is not HELLO: $(frames "$tmp/f.out" | head -n 1)"
grep -qE "$update_re" "$tmp/f.out" || { cat "$tmp/f.out"; fail "(f) no UPDATE line"; }
grep -q 'TCP listener disabled' "$tmp/skycued-out.log" || { cat "$tmp/skycued-out.log"; fail "(f) skycued listened"; }

echo "PASS"
