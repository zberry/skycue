#!/usr/bin/env bash
# skycue-dump against a real skycued replaying the readsb JSON fixture: text
# and --json output over TCP, text output over UDP, exit 3 when nothing
# arrives before --timeout, and heartbeats counting towards --count.
# usage: test_dump.sh SKYCUED SKYCUE_DUMP FIXTURE_DIR
set -u
skycued=$1 dump=$2 fixtures=$3
replay="$fixtures/readsb-sample.jsonl"
tmp=$(mktemp -d)
bg_pids=()
cleanup() {
  [ ${#bg_pids[@]} -gt 0 ] && kill "${bg_pids[@]}" 2>/dev/null
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
cat > "$tmp/skycue.conf" <<EOF
RECEIVER_LAT=38.9
RECEIVER_LON=-77.0
CUE_TCP_PORT=$tcp_port
CUE_UDP_TARGETS=127.0.0.1:$udp_port
EOF
"$skycued" --replay "$replay" --replay-speed 50 --replay-loop --config "$tmp/skycue.conf" \
  >"$tmp/skycued.log" 2>&1 &
bg_pids+=($!)

# ---- (a) text over TCP: exactly --count lines, HELLO first, some UPDATEs ----
ts='^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z '
timeout 30 "$dump" --tcp "127.0.0.1:$tcp_port" --count 20 --timeout 15 >"$tmp/a.out" 2>"$tmp/a.err"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/a.err"; fail "(a) exit $rc"; }
n=$(wc -l < "$tmp/a.out")
[ "$n" -eq 20 ] || { cat "$tmp/a.out"; fail "(a) $n lines, want 20"; }
head -n 1 "$tmp/a.out" | grep -qE "${ts}HELLO skycued kf=[0-9]+ms hb=[0-9]+ms$" ||
  fail "(a) first line is not HELLO: $(head -n 1 "$tmp/a.out")"
grep -qE "${ts}UPDATE [0-9A-F]{6} [a-z_]+ [a-z0-9_]+ -?[0-9]+\.[0-9]{7} -?[0-9]+\.[0-9]{7}( |$)" "$tmp/a.out" ||
  { cat "$tmp/a.out"; fail "(a) no UPDATE line"; }
bad=$(grep -cvE "${ts}(HELLO|UPDATE|DROP|HB|CONN)( |$)" "$tmp/a.out")
[ "$bad" -eq 0 ] || { cat "$tmp/a.out"; fail "(a) $bad malformed lines"; }

# ---- (b) --json over TCP: one JSON object per line ---------------------------
timeout 30 "$dump" --tcp "127.0.0.1:$tcp_port" --json --count 10 --timeout 15 >"$tmp/b.out" 2>"$tmp/b.err"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/b.err"; fail "(b) exit $rc"; }
python3 -I -c 'import json,sys
n=0
for l in sys.stdin:
    o=json.loads(l); n+=1
    assert isinstance(o["time"], str) and o["frame"] in ("hello","update","drop","heartbeat","conn"), l
assert n==10, n' < "$tmp/b.out" || { cat "$tmp/b.out"; fail "(b) JSON check"; }
head -n 1 "$tmp/b.out" | grep -q '"frame":"hello"' || fail "(b) first object is not hello"

# ---- (c) text over UDP --------------------------------------------------------
timeout 30 "$dump" --udp "127.0.0.1:$udp_port" --count 5 --timeout 15 >"$tmp/c.out" 2>"$tmp/c.err"
rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/c.err"; fail "(c) exit $rc"; }
n=$(wc -l < "$tmp/c.out")
[ "$n" -eq 5 ] || { cat "$tmp/c.out"; fail "(c) $n lines, want 5"; }
bad=$(grep -cvE "${ts}(HELLO|UPDATE|DROP|HB|CONN)( |$)" "$tmp/c.out")
[ "$bad" -eq 0 ] || { cat "$tmp/c.out"; fail "(c) $bad malformed lines"; }

# ---- (d) nothing to receive: exit 3 after --timeout, no output ----------------
dead_port=$(free_port tcp)
timeout 30 "$dump" --tcp "127.0.0.1:$dead_port" --timeout 1 >"$tmp/d.out" 2>"$tmp/d.err"
rc=$?
[ "$rc" -eq 3 ] || { cat "$tmp/d.err"; fail "(d) exit $rc, want 3"; }
[ ! -s "$tmp/d.out" ] || fail "(d) unexpected output"

# ---- (e) heartbeats are frames for --count: Hello + 3 Heartbeats = 4 lines ---
# (skycued under replay is never idle, so a python peer sends the heartbeats.)
cat > "$tmp/hb.py" <<'EOF'
import socket, struct, sys, os, time
def frame(t, p): return b"SKYC" + struct.pack("<BBH", 1, t, len(p)) + p
s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(1)
open(sys.argv[1] + ".tmp", "w").write(str(s.getsockname()[1])); os.rename(sys.argv[1] + ".tmp", sys.argv[1])
s.settimeout(20); c, _ = s.accept()
c.sendall(frame(1, struct.pack("<II", 10000, 1000) + b"\x04test"))
for i in range(3): c.sendall(frame(4, struct.pack("<Q", 1000 * i)))
time.sleep(5)
EOF
rm -f "$tmp/hbport"
python3 -I "$tmp/hb.py" "$tmp/hbport" &
hb_pid=$!
bg_pids+=($hb_pid)
for (( i=0; i<50; i++ )); do [ -s "$tmp/hbport" ] && break; sleep 0.1; done
[ -s "$tmp/hbport" ] || fail "(e) heartbeat peer did not start"
timeout 30 "$dump" --tcp "127.0.0.1:$(cat "$tmp/hbport")" --count 4 --timeout 10 >"$tmp/e.out" 2>"$tmp/e.err"
rc=$?
kill "$hb_pid" 2>/dev/null
[ "$rc" -eq 0 ] || { cat "$tmp/e.err"; fail "(e) exit $rc"; }
got=$(sed -E 's/^[^ ]+ //' "$tmp/e.out" | tr '\n' '|')
[ "$got" = "HELLO test kf=10000ms hb=1000ms|HB|HB|HB|" ] || fail "(e) got '$got'"

echo "PASS"
