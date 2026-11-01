#!/usr/bin/env bash
# skycue-rawplay and skycue-rawrec against python3 socket peers, on the
# committed real-data fixtures; then (when readsb is built) rawplay into a
# real readsb and read its JSON position output.
# usage: test_rawtools.sh RAWREC RAWPLAY FIXTURE_DIR [READSB]
set -u
rawrec=$1 rawplay=$2 fixtures=$3 readsb=${4:-}
csv="$fixtures/raw/modes-120s.csv.gz"
uat="$fixtures/raw/uat-sample.txt.gz"
tmp=$(mktemp -d)
bg_pids=()
cleanup() {
  [ ${#bg_pids[@]} -gt 0 ] && kill "${bg_pids[@]}" 2>/dev/null
  wait 2>/dev/null
  rm -rf "$tmp"
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; exit 1; }
for f in "$rawrec" "$rawplay" "$csv" "$uat"; do [ -e "$f" ] || fail "$f missing"; done

now() { date +%s.%N; }
wait_file() { # wait_file FILE TENTHS: until FILE is non-empty
  local i
  for (( i=0; i<$2; i++ )); do [ -s "$1" ] && return 0; sleep 0.1; done
  return 1
}

# sink.py PORTFILE OUT: accept one TCP connection on an ephemeral port and
# store everything received until EOF.
cat > "$tmp/sink.py" <<'EOF'
import socket, sys
s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(1)
open(sys.argv[1] + ".tmp", "w").write(str(s.getsockname()[1]))
import os; os.rename(sys.argv[1] + ".tmp", sys.argv[1])
c, _ = s.accept()
with open(sys.argv[2], "wb") as out:
    while True:
        b = c.recv(65536)
        if not b: break
        out.write(b)
EOF
# fetch.py PORT OUT: connect (retrying for 10 s) and store everything until EOF.
cat > "$tmp/fetch.py" <<'EOF'
import socket, sys, time
deadline = time.time() + 10
while True:
    try:
        c = socket.create_connection(("127.0.0.1", int(sys.argv[1]))); break
    except OSError:
        if time.time() > deadline: sys.exit("fetch: cannot connect")
        time.sleep(0.05)
with open(sys.argv[2], "wb") as out:
    while True:
        b = c.recv(65536)
        if not b: break
        out.write(b)
EOF
free_port() {
  python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'
}
# play OUT ARGS...: rawplay ARGS with --raw-in/--beast-in pointed at a sink; prints the wall time.
play_to_sink() {
  local out=$1 opt=$2; shift 2
  rm -f "$tmp/port" "$out"
  python3 "$tmp/sink.py" "$tmp/port" "$out" & local sink=$!
  wait_file "$tmp/port" 50 || fail "sink did not start"
  local t0 t1
  t0=$(now)
  timeout 60 "$rawplay" "$opt" "127.0.0.1:$(cat "$tmp/port")" "$@" 2>"$tmp/play.err" || { cat "$tmp/play.err"; fail "rawplay $* exit $?"; }
  t1=$(now)
  wait "$sink"
  awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f\n", b - a }'
}

# ---- (a) CSV at speed 0: every capture line becomes an "@" AVR line ---------
play_to_sink "$tmp/a.out" --raw-in --speed 0 "$csv" >/dev/null
total=$(wc -l < "$tmp/a.out")
good=$(grep -cE '^@[0-9A-F]{12}[0-9A-F]{14,28};$' "$tmp/a.out")
want=$(zcat "$csv" | wc -l)
[ "$good" -ge 170000 ] || fail "(a) only $good matching lines"
[ "$good" = "$total" ] || fail "(a) $((total - good)) of $total lines malformed"
[ "$good" = "$want" ] || fail "(a) $good lines, capture has $want"
# The first line carries the capture's own epoch: 1783963099.005369 s * 12 MHz mod 2^48.
first=$(head -n 1 "$tmp/a.out")
zcat "$csv" | head -n 1 | python3 -c '
import sys
ts, hexmsg = sys.stdin.read().strip().split(",")
sec, frac = ts.split(".")
ns = int(sec) * 10**9 + int((frac + "000000000")[:9])
print("@%012X%s;" % ((ns * 12 // 1000) % (1 << 48), hexmsg.upper()))' > "$tmp/a.want"
[ "$first" = "$(cat "$tmp/a.want")" ] || fail "(a) first line '$first', want '$(cat "$tmp/a.want")'"
echo "ok (a) $good AVR lines"

# ---- (b) pacing by the capture's epochs ---------------------------------------
pace_check() { # pace_check LINES SPEED
  zcat "$csv" | head -n "$1" > "$tmp/head.csv"
  local span wall
  span=$(awk -F, 'NR==1{f=$1} {l=$1} END{printf "%.6f", l-f}' "$tmp/head.csv")
  wall=$(play_to_sink "$tmp/b.out" --raw-in --speed "$2" "$tmp/head.csv")
  [ "$(wc -l < "$tmp/b.out")" = "$1" ] || fail "(b) $1 lines at speed $2: got $(wc -l < "$tmp/b.out")"
  awk -v w="$wall" -v s="$span" -v k="$2" 'BEGIN { exit !(w >= s / k * 0.8 && w <= s / k + 1.5) }' ||
    fail "(b) $1 lines at speed $2: wall ${wall}s for a ${span}s span"
  echo "ok (b) $1 lines at speed $2: wall ${wall}s, span ${span}s"
}
pace_check 500 50
pace_check 5000 5

# ---- (c) UAT lines to a client of --uat-listen --------------------------------
play_uat() { # play_uat OUT ARGS...: prints wall time
  local out=$1; shift
  local port; port=$(free_port)
  local t0 t1
  python3 "$tmp/fetch.py" "$port" "$out" & local fetch=$!
  t0=$(now)
  timeout 60 "$rawplay" --uat-listen "$port" "$@" 2>"$tmp/play.err" || { cat "$tmp/play.err"; fail "rawplay --uat-listen $* exit $?"; }
  t1=$(now)
  wait "$fetch" || fail "(c) fetch failed"
  awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f\n", b - a }'
}
play_uat "$tmp/c.out" --speed 0 "$uat" >/dev/null
zcat "$uat" | grep -E '^[-+]' > "$tmp/c.want"
[ "$(wc -l < "$tmp/c.out")" = "$(wc -l < "$tmp/c.want")" ] ||
  fail "(c) $(wc -l < "$tmp/c.out") UAT lines, want $(wc -l < "$tmp/c.want")"
cmp -s "$tmp/c.out" "$tmp/c.want" || fail "(c) UAT lines differ from the file's"
echo "ok (c) $(wc -l < "$tmp/c.out") UAT lines"
# Untimestamped: 10 lines/s times the speed; 30 lines at speed 5 ~ 0.6 s.
head -n 30 "$tmp/c.want" > "$tmp/c30.txt"
wall=$(play_uat "$tmp/c30.out" --speed 5 "$tmp/c30.txt")
cmp -s "$tmp/c30.out" "$tmp/c30.txt" || fail "(c) 30 untimestamped lines differ"
awk -v w="$wall" 'BEGIN { exit !(w >= 0.45 && w <= 2.5) }' || fail "(c) 30 lines at speed 5 took ${wall}s"
# Timestamped (as rawrec writes them): prefixes stripped, paced by them (1.2 s span).
awk '{ printf "%d %s\n", 1793536496000 + (NR - 1) * 200, $0 }' <(head -n 7 "$tmp/c.want") > "$tmp/uat-ts.txt"
head -n 7 "$tmp/c.want" > "$tmp/uat-ts.want"
wall=$(play_uat "$tmp/c7.out" "$tmp/uat-ts.txt")
cmp -s "$tmp/c7.out" "$tmp/uat-ts.want" || fail "(c) timestamped lines not stripped"
awk -v w="$wall" 'BEGIN { exit !(w >= 0.96 && w <= 3.0) }' || fail "(c) 1.2 s of timestamped lines took ${wall}s"
echo "ok (c) pacing: untimestamped and timestamped"

# ---- (d) rawrec: byte-identical Beast capture across a reconnect; UAT lines ----
# beast_server.py PORTFILE SENT FRAMES CONNS: serve CONNS connections, each
# sent FRAMES/CONNS synthetic frames (escaped 0x1a included) in small chunks.
cat > "$tmp/beast_server.py" <<'EOF'
import os, random, socket, sys, time
portfile, sent, nframes, conns = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
rnd = random.Random(7)
def esc(b): return b.replace(b"\x1a", b"\x1a\x1a")
frames = []
for i in range(nframes):
    t = rnd.choice(b"123")
    n = {ord("1"): 2, ord("2"): 7, ord("3"): 14}[t]
    ts = (i * 12000 + (0x1a if i % 5 == 0 else 0)).to_bytes(6, "big")
    body = ts + bytes([0x1a if i % 3 == 0 else 0x40]) + bytes(rnd.choice([0x1a, rnd.randrange(256)]) for _ in range(n))
    frames.append(b"\x1a" + bytes([t]) + esc(body))
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(1)
open(portfile + ".tmp", "w").write(str(s.getsockname()[1])); os.rename(portfile + ".tmp", portfile)
with open(sent, "wb") as out:
    per = (nframes + conns - 1) // conns
    for k in range(conns):
        c, _ = s.accept()
        part = b"".join(frames[k * per:(k + 1) * per])
        out.write(part); out.flush()
        for j in range(0, len(part), 1000):  # several reads, cutting frames anywhere
            c.sendall(part[j:j + 1000]); time.sleep(0.002)
        c.close()
s.close()
EOF
cat > "$tmp/uat_server.py" <<'EOF'
import os, socket, sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(1)
open(sys.argv[1] + ".tmp", "w").write(str(s.getsockname()[1])); os.rename(sys.argv[1] + ".tmp", sys.argv[1])
c, _ = s.accept()
c.sendall(b"-00a66ef1;rs=1;\n+3514c952;\n-00a6")  # the last line is cut by the close
c.close(); s.close()
EOF
rawrec_run() { # rawrec_run DIR FRAMES CONNS [ARGS...]: capture, then SIGTERM; checks exit 0
  local dir=$1 nframes=$2 conns=$3; shift 3
  rm -f "$tmp/bport" "$tmp/uport"
  python3 "$tmp/beast_server.py" "$tmp/bport" "$tmp/sent.bin" "$nframes" "$conns" & local bs=$!
  python3 "$tmp/uat_server.py" "$tmp/uport" & local us=$!
  wait_file "$tmp/bport" 50 && wait_file "$tmp/uport" 50 || fail "(d) servers did not start"
  "$rawrec" --beast "127.0.0.1:$(cat "$tmp/bport")" --uat "127.0.0.1:$(cat "$tmp/uport")" --dir "$dir" "$@" \
    2>"$tmp/rec.err" & local rec=$!
  bg_pids+=("$rec")
  wait "$bs" || fail "(d) beast server failed"
  wait "$us"
  local i
  for (( i=0; i<50; i++ )); do
    [ "$(cat "$dir"/beast-*.bin 2>/dev/null | wc -c)" = "$(wc -c < "$tmp/sent.bin")" ] && break
    sleep 0.1
  done
  kill -TERM "$rec"
  wait "$rec"; local rc=$?
  [ $rc -eq 0 ] || { cat "$tmp/rec.err"; fail "(d) rawrec exit $rc after SIGTERM"; }
}
rawrec_run "$tmp/rec" 100 2
ls "$tmp/rec"/beast-*.bin >/dev/null 2>&1 || { cat "$tmp/rec.err"; fail "(d) no beast file"; }
[ "$(ls "$tmp/rec"/beast-*.bin | wc -l)" = 1 ] || fail "(d) more than one beast file"
ls "$tmp/rec"/beast-*.bin | grep -qE '/beast-[0-9]{8}-[0-9]{6}\.bin$' || fail "(d) beast file name: $(ls "$tmp/rec")"
cmp "$tmp/rec"/beast-*.bin "$tmp/sent.bin" || fail "(d) capture differs from what was sent"
uatf=$(ls "$tmp/rec"/uat-*.txt 2>/dev/null) || fail "(d) no uat file"
echo "$uatf" | grep -qE '/uat-[0-9]{8}-[0-9]{6}\.txt$' || fail "(d) uat file name: $uatf"
[ "$(wc -l < "$uatf")" = 2 ] || { cat "$uatf"; fail "(d) want 2 UAT lines"; }
sed -n 1p "$uatf" | grep -qE '^1[0-9]{12} -00a66ef1;rs=1;$' || { cat "$uatf"; fail "(d) UAT line 1"; }
sed -n 2p "$uatf" | grep -qE '^1[0-9]{12} \+3514c952;$' || { cat "$uatf"; fail "(d) UAT line 2"; }
grep -q 'connected' "$tmp/rec.err" || fail "(d) rawrec logged no connection"
# What rawrec wrote replays: the file goes back out byte for byte.
play_to_sink "$tmp/d.out" --beast-in --speed 0 "$tmp/rec"/beast-*.bin >/dev/null
cmp -s "$tmp/d.out" "$tmp/sent.bin" || fail "(d) rawplay of the capture differs"
echo "ok (d) rawrec capture byte-identical; rawplay replays it"

# ---- (e) rotation by size, at frame boundaries ---------------------------------
rawrec_run "$tmp/rot" 1000 1 --rotate-mb 0.01
n=$(ls "$tmp/rot"/beast-*.bin | wc -l)
[ "$n" -ge 2 ] || fail "(e) rotate-mb 0.01 gave $n beast file(s)"
# Creation order: by time stamp, then -N suffix (a plain sort puts "-1.bin" before ".bin").
in_order() {
  python3 -c '
import re, sys
def key(p):
    m = re.search(r"-(\d{8}-\d{6})(?:-(\d+))?\.\w+$", p)
    return (m.group(1), int(m.group(2) or 0))
print("\n".join(sorted(sys.argv[1:], key=key)))' "$@"
}
cat $(in_order "$tmp/rot"/beast-*.bin) | cmp -s - "$tmp/sent.bin" || fail "(e) rotated files do not concatenate to the stream"
for f in "$tmp/rot"/beast-*.bin; do
  head -c 1 "$f" | od -An -tx1 | grep -q 1a || fail "(e) $f does not start at a frame"
  [ "$(wc -c < "$f")" -le $((10486 + 64)) ] || fail "(e) $f is $(wc -c < "$f") bytes"
done
echo "ok (e) $n rotated files"

# ---- full stack: the capture through a real readsb -----------------------------
if [ -z "$readsb" ] || [ ! -x "$readsb" ]; then
  echo "SKIP full-stack check: readsb not built (${readsb:-no path given})"
  echo PASS
  exit 0
fi
ri=$(free_port); jp=$(free_port)
LD_LIBRARY_PATH="$(dirname "$(dirname "$readsb")")/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$readsb" --net-only --net-ri-port "$ri" --net-json-port "$jp" --lat 52.0 --lon 4.5 --max-range 500 --quiet \
  >"$tmp/readsb.log" 2>&1 & rs=$!
bg_pids+=("$rs")
python3 - "$jp" "$tmp/json.out" > "$tmp/json.count" <<'EOF' & reader=$!
import socket, sys, time
deadline = time.time() + 10
while True:
    try:
        c = socket.create_connection(("127.0.0.1", int(sys.argv[1]))); break
    except OSError:
        if time.time() > deadline: sys.exit("json: cannot connect")
        time.sleep(0.1)
c.settimeout(1)
buf = b""; n = 0; end = time.time() + 60
with open(sys.argv[2], "wb") as out:
    while time.time() < end and n < 200:
        try:
            b = c.recv(65536)
        except socket.timeout:
            continue
        if not b: break
        out.write(b); buf += b
        *lines, buf = buf.split(b"\n")
        n += sum(1 for l in lines if b'"lat"' in l)
print(n)
EOF
sleep 0.5
timeout 60 "$rawplay" --raw-in "127.0.0.1:$ri" --speed 0 "$csv" 2>"$tmp/play.err" || { cat "$tmp/play.err"; fail "full stack: rawplay exit $?"; }
wait "$reader" || { cat "$tmp/readsb.log"; fail "full stack: JSON reader failed"; }
n=$(cat "$tmp/json.count")
hexes=$(grep -o '"hex":"[^"]*"' "$tmp/json.out" | sort -u | wc -l)
[ "$n" -ge 50 ] || { tail -n 20 "$tmp/readsb.log"; fail "full stack: only $n JSON lines with lat"; }
echo "ok full stack: readsb sent $n JSON position lines for $hexes aircraft"
echo PASS
