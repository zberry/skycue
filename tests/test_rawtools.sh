#!/usr/bin/env bash
# skycue-rawplay and skycue-rawrec against python3 socket peers, on the
# committed real-data fixtures; then (when readsb is built) rawplay into a
# real readsb and read its JSON position output.
# usage: test_rawtools.sh RAWREC RAWPLAY FIXTURE_DIR [READSB]
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib/test_common.sh"
rawrec=$1 rawplay=$2 fixtures=$3 readsb=${4:-}
csv="$fixtures/raw/modes-120s.csv.gz"
uat="$fixtures/raw/uat-sample.txt.gz"
stratux="$fixtures/raw/stratux-uat-2015.log.gz"
test_init rawtools
# fixtures/raw/ is fetched by scripts/fixtures/fetch-test-captures.sh (fixtures/README.md):
# without it the sections that replay those captures, (a)-(c2) and the full
# stack, are skipped and the rest still runs.
have_raw=1
for f in "$csv" "$uat" "$stratux"; do
  [ -e "$f" ] || { have_raw=0; echo "SKIP (a)-(c2) and full stack: $f not fetched: run scripts/fixtures/fetch-test-captures.sh"; break; }
done
for f in "$rawrec" "$rawplay"; do [ -e "$f" ] || fail "$f missing"; done

now() { date +%s.%N; }

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
import os, socket, sys, time
slowf = float(os.environ.get("SKYCUE_TEST_SLOW", "1"))
deadline = time.time() + 10 * slowf
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
# play OUT ARGS...: rawplay ARGS with --raw-in/--beast-in pointed at a sink; prints the wall time.
play_to_sink() {
  local out=$1 opt=$2; shift 2
  rm -f "$tmp/port" "$out"
  python3 "$tmp/sink.py" "$tmp/port" "$out" & local sink=$!
  wait_file "$tmp/port" 5 || fail "sink did not start"
  local t0 t1
  t0=$(now)
  timeout $((60*slowf)) "$rawplay" "$opt" "127.0.0.1:$(cat "$tmp/port")" "$@" 2>"$tmp/play.err" || { cat "$tmp/play.err"; fail "rawplay $* exit $?"; }
  t1=$(now)
  wait "$sink"
  awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f\n", b - a }'
}

if [ "$have_raw" = 1 ]; then  # (a)-(c2) replay fixtures/raw/
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
  timeout $((60*slowf)) "$rawplay" --uat-listen "$port" "$@" 2>"$tmp/play.err" || { cat "$tmp/play.err"; fail "rawplay --uat-listen $* exit $?"; }
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

# ---- (c2) stratux "<ns>," lines: prefix stripped, paced by the offsets ----------
# The fixture's first 7 lines with their offsets replaced by 0, 200 ms, ..., 1.2 s,
# after its "START,..." header and the blank lines between records.
zcat "$stratux" | grep -E '^[0-9]+,[-+]' | head -n 7 > "$tmp/stx7.src"
[ "$(wc -l < "$tmp/stx7.src")" -eq 7 ] || fail "(c2) fixture has fewer than 7 stratux lines"
{ echo "START,Wed Sep 2 04:23:37 +0000 UTC 2015"
  awk -F, '{ printf "%d,%s\n\n", (NR - 1) * 200000000, substr($0, index($0, ",") + 1) }' "$tmp/stx7.src"
} > "$tmp/stx7.log"
cut -d, -f2- "$tmp/stx7.src" > "$tmp/stx7.want"
wall=$(play_uat "$tmp/stx7.out" --speed 1 "$tmp/stx7.log")
cmp -s "$tmp/stx7.out" "$tmp/stx7.want" || { head -c 300 "$tmp/stx7.out"; fail "(c2) stratux lines not stripped to their dump978 text"; }
awk -v w="$wall" 'BEGIN { exit !(w >= 1.1 && w <= 3.0) }' || fail "(c2) 1.2 s of stratux lines took ${wall}s"
grep -q '1 lines skipped' "$tmp/play.err" || { cat "$tmp/play.err"; fail "(c2) the START line was not counted as skipped"; }
echo "ok (c2) stratux lines paced by their offsets (${wall}s)"
# Two sessions: lines 1-4 at 0..600 ms, a START, lines 5-7 at 100/400/700 ms
# (below session 1's). In file order, session 2 starts 1 s after line 4 and
# keeps its own spacing: lines at 1.6/1.9/2.2 s, so >= 2.1 s in all. A burst
# would end at 0.6 s; session 2 sent at once after the gap, at 1.6 s.
{ echo "START,Wed Sep 2 04:23:37 +0000 UTC 2015"
  awk '{ off = NR <= 4 ? (NR - 1) * 200 : 100 + (NR - 5) * 300
         if (NR == 5) print "START,Wed Sep 2 04:25:25 +0000 UTC 2015"
         printf "%d,%s\n\n", off * 1000000, substr($0, index($0, ",") + 1) }' "$tmp/stx7.src"
} > "$tmp/stx2s.log"
wall=$(play_uat "$tmp/stx2s.out" --speed 1 "$tmp/stx2s.log")
cmp -s "$tmp/stx2s.out" "$tmp/stx7.want" || { head -c 300 "$tmp/stx2s.out"; fail "(c2) two sessions not replayed in file order"; }
awk -v w="$wall" 'BEGIN { exit !(w >= 2.1 && w <= 4.0) }' || fail "(c2) two sessions (2.2 s) took ${wall}s"
echo "ok (c2) two stratux sessions in file order, 1 s apart, no burst (${wall}s)"
fi  # have_raw

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
  wait_file "$tmp/bport" 5 && wait_file "$tmp/uport" 5 || fail "(d) servers did not start"
  "$rawrec" --beast "127.0.0.1:$(cat "$tmp/bport")" --uat "127.0.0.1:$(cat "$tmp/uport")" --dir "$dir" "$@" \
    2>"$tmp/rec.err" & local rec=$!
  bg_pids+=("$rec")
  wait "$bs" || fail "(d) beast server failed"
  wait "$us"
  local i
  for (( i=0; i<50*slowf; i++ )); do
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

# ---- (d2) rawrec endpoint words: tcp://HOST:PORT, and udp://BIND:PORT datagrams -----
rm -f "$tmp/uport"
python3 "$tmp/uat_server.py" "$tmp/uport" & us=$!
wait_file "$tmp/uport" 5 || fail "(d2) uat server did not start"
uport=$(free_port)
"$rawrec" --uat "tcp://127.0.0.1:$(cat "$tmp/uport")" --dir "$tmp/ep" 2>"$tmp/ep.err" & rec=$!
bg_pids+=("$rec")
wait "$us"
wait_for 5 test -s "$tmp/ep"/uat-*.txt 2>/dev/null
slow_sleep 0.3
kill -TERM "$rec"; wait "$rec" || fail "(d2) rawrec tcp:// exit $?"
[ "$(wc -l < "$tmp/ep"/uat-*.txt)" = 2 ] || { cat "$tmp/ep"/uat-*.txt "$tmp/ep.err"; fail "(d2) tcp:// recorded the wrong number of lines"; }
"$rawrec" --uat "udp://127.0.0.1:$uport" --dir "$tmp/epu" 2>"$tmp/epu.err" & rec=$!
bg_pids+=("$rec")
# Datagrams of two lines each, repeated: the first ones may come before rawrec has bound.
python3 - "$uport" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for _ in range(30):
    s.sendto(b"-00a66ef1;rs=1;\n+3514c952;\n", ("127.0.0.1", int(sys.argv[1])))
    time.sleep(0.05)
PY
slow_sleep 0.3
kill -TERM "$rec"; wait "$rec" || fail "(d2) rawrec udp:// exit $?"
n=$(cat "$tmp/epu"/uat-*.txt | wc -l)
[ "$n" -ge 2 ] && [ $((n % 2)) = 0 ] || { cat "$tmp/epu"/uat-*.txt "$tmp/epu.err"; fail "(d2) udp:// recorded $n lines"; }
grep -qE '^1[0-9]{12} -00a66ef1;rs=1;$' "$tmp/epu"/uat-*.txt || fail "(d2) udp:// line format"
"$rawrec" --uat "udp://127.0.0.1" --dir "$tmp/epx" >/dev/null 2>&1
[ $? -eq 2 ] || fail "(d2) a udp:// endpoint without a port should exit 2"
echo "ok (d2) rawrec tcp:// and udp:// endpoints"

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

# ---- (f) write failures: backoff, at most one extra file per stream ----------
# Every write fails (test seam) while ~2 s of Beast data arrives; with a 20 ms
# backoff doubling to 80 ms that is dozens of reopens, yet the streak reuses
# one name: the first file plus one per stream, and one ERROR per stream.
rm -f "$tmp/bport" "$tmp/uport"
python3 "$tmp/beast_server.py" "$tmp/bport" "$tmp/sent.bin" 40000 1 & bs=$!
python3 "$tmp/uat_server.py" "$tmp/uport" & us=$!
wait_file "$tmp/bport" 5 && wait_file "$tmp/uport" 5 || fail "(f) servers did not start"
"$rawrec" --beast "127.0.0.1:$(cat "$tmp/bport")" --uat "127.0.0.1:$(cat "$tmp/uport")" --dir "$tmp/fail" \
  --fail-writes-for-test --reopen-ms-for-test 20 2>"$tmp/fail.err" & rec=$!
bg_pids+=("$rec")
t0=$(now)
wait "$bs" || fail "(f) beast server failed"
wait "$us"
slow_sleep 0.3
kill -TERM "$rec"
wait "$rec"; rc=$?
[ $rc -eq 0 ] || { cat "$tmp/fail.err"; fail "(f) rawrec exit $rc after SIGTERM"; }
span=$(awk -v a="$t0" -v b="$(now)" 'BEGIN { printf "%.1f", b - a }')
awk -v s="$span" 'BEGIN { exit !(s >= 1.0) }' || fail "(f) the Beast data took only ${span}s; too short to retry often"
nb=$(ls "$tmp/fail"/beast-*.bin 2>/dev/null | wc -l)
nu=$(ls "$tmp/fail"/uat-*.txt 2>/dev/null | wc -l)
[ "$nb" -le 2 ] && [ "$nu" -le 2 ] || { ls -l "$tmp/fail"; fail "(f) $nb beast and $nu uat files after many failed writes, want <= 2 each"; }
[ "$nb" = 2 ] || { ls -l "$tmp/fail"; cat "$tmp/fail.err"; fail "(f) $nb beast files: no reopen happened"; }
[ "$(grep -c 'ERROR beast: .*dropping data, retrying in 0.02 s, backing off to 0.08 s' "$tmp/fail.err")" = 1 ] ||
  { cat "$tmp/fail.err"; fail "(f) want exactly one ERROR for the beast streak"; }
[ "$(grep -c 'ERROR uat: ' "$tmp/fail.err")" -le 1 ] || { cat "$tmp/fail.err"; fail "(f) more than one uat ERROR"; }
grep -q 'writing again' "$tmp/fail.err" && fail "(f) recovered although every write fails"
echo "ok (f) $nb beast / $nu uat files over ${span}s of failing writes"

# ---- (g) --max-gb: retention over --retention-dir and its raw/ ----------------
mkdir -p "$tmp/ret/raw"
head -c 30000 /dev/zero > "$tmp/ret/raw/beast-20200101-000000.bin"
head -c 30000 /dev/zero > "$tmp/ret/received-20200101-000000.jsonl"
head -c 100 /dev/zero > "$tmp/ret/received-20200102-000000.jsonl"  # newest of its stream: kept
head -c 50000 /dev/zero > "$tmp/ret/notes.txt"                     # not a recording: kept, not counted
# A day old: files modified within the last 10 minutes are never deleted.
touch -d '1 day ago' "$tmp/ret/raw/beast-20200101-000000.bin" "$tmp/ret/received-20200101-000000.jsonl" \
  "$tmp/ret/received-20200102-000000.jsonl" "$tmp/ret/notes.txt"
rawrec_run "$tmp/ret/raw" 1000 1 --rotate-mb 0.005 --max-gb 0.00003 --retention-dir "$tmp/ret"
[ -e "$tmp/ret/raw/beast-20200101-000000.bin" ] && { cat "$tmp/rec.err"; fail "(g) old beast file not deleted"; }
[ -e "$tmp/ret/received-20200101-000000.jsonl" ] && { cat "$tmp/rec.err"; fail "(g) old received file not deleted"; }
[ -e "$tmp/ret/received-20200102-000000.jsonl" ] || fail "(g) the newest received file was deleted"
[ -e "$tmp/ret/notes.txt" ] || fail "(g) notes.txt was deleted"
cat $(in_order "$tmp/ret/raw"/beast-*.bin) | cmp -s - "$tmp/sent.bin" || fail "(g) the capture lost data to retention"
[ "$(grep -c 'INFO  retention: deleted' "$tmp/rec.err")" = 2 ] || { cat "$tmp/rec.err"; fail "(g) want 2 retention INFO lines"; }
echo "ok (g) retention deleted the 2 oldest files"

# ---- (h) a .bin with a frame cut short (a write failure's gap) replays -----------
# 50 frames, a long frame cut after 9 bytes (no escape pair split), 60 frames:
# rawplay skips the cut bytes and sends the other 110 frames unchanged.
python3 - "$tmp/gap.bin" "$tmp/gap.want" <<'EOF'
import random, sys
rnd = random.Random(11)
def esc(b): return b.replace(b"\x1a", b"\x1a\x1a")
def frame(i):
    t = b"123"[i % 3]
    n = {ord("1"): 2, ord("2"): 7, ord("3"): 14}[t]
    body = (i * 12000).to_bytes(6, "big") + bytes([0x40]) + bytes(rnd.choice([0x1a, rnd.randrange(256)]) for _ in range(n))
    return b"\x1a" + bytes([t]) + esc(body)
good = [frame(i) for i in range(110)]
cut = b"\x1a3" + bytes([0, 0, 0, 1, 2, 3, 0x40])  # 9 of 23 bytes, no 0x1a in the body
open(sys.argv[1], "wb").write(b"".join(good[:50]) + cut + b"".join(good[50:]))
open(sys.argv[2], "wb").write(b"".join(good))
EOF
play_to_sink "$tmp/h.out" --beast-in --speed 0 "$tmp/gap.bin" >/dev/null
cmp -s "$tmp/h.out" "$tmp/gap.want" || fail "(h) replay of the gapped capture is not the 110 good frames"
grep -q 'done: 110 frames sent, 9 bytes skipped' "$tmp/play.err" || { cat "$tmp/play.err"; fail "(h) rawplay summary"; }
echo "ok (h) rawplay resyncs after a truncated frame: 110 frames, 9 bytes skipped"

# ---- (i) rawplay serves several clients --------------------------------------------
# These use generated files, not fixtures/raw/. fetch.py (above) takes the port and
# the output file; delay.py is the same after a pause; mute.py connects and never reads.
cat > "$tmp/mute.py" <<'PY'
import socket, sys, time
deadline = time.time() + 10
while True:
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)  # a small window: the server's queue fills early
    try:
        s.connect(("127.0.0.1", int(sys.argv[1]))); break
    except OSError:
        s.close()
        if time.time() > deadline: sys.exit("mute: cannot connect")
        time.sleep(0.05)
time.sleep(300)
PY
uat_lines() { # uat_lines N WIDTH STEP_MS: "<epoch_ms> -<WIDTH x a>;" lines, STEP_MS apart
  awk -v n="$1" -v w="$2" -v step="$3" 'BEGIN { pad = sprintf("%*s", w, ""); gsub(/ /, "a", pad)
    for (i = 0; i < n; i++) printf "%d -%s;\n", 1793536496000 + i * step, i % 10 pad }'
}

# two_clients_same_frames: both clients of one listener get the same bytes, for
# UAT lines, for a .bin as recorded and for a .csv as Beast frames.
uat_lines 300 90 5 > "$tmp/i.txt"
sed 's/^[0-9]* //' "$tmp/i.txt" > "$tmp/i.want"
p1=$(free_port)
python3 "$tmp/fetch.py" "$p1" "$tmp/i1.out" & f1=$!
python3 "$tmp/fetch.py" "$p1" "$tmp/i2.out" & f2=$!
timeout $((60*slowf)) "$rawplay" --uat-listen "$p1" --wait-clients 2 --speed 4 "$tmp/i.txt" 2>"$tmp/play.err" ||
  { cat "$tmp/play.err"; fail "(i) two clients: rawplay exit $?"; }
wait "$f1" "$f2" || fail "(i) a client failed"
cmp -s "$tmp/i1.out" "$tmp/i.want" || fail "(i) client 1 lines differ from the file's"
cmp -s "$tmp/i2.out" "$tmp/i.want" || fail "(i) client 2 lines differ from the file's"
grep -q 'uat: done: 300 lines sent, 0 lines skipped, 0 reconnects, 0 bytes dropped' "$tmp/play.err" ||
  { cat "$tmp/play.err"; fail "(i) uat summary"; }
p1=$(free_port)
python3 "$tmp/fetch.py" "$p1" "$tmp/i1.out" & f1=$!
python3 "$tmp/fetch.py" "$p1" "$tmp/i2.out" & f2=$!
timeout $((60*slowf)) "$rawplay" --1090-listen "$p1" --wait-clients 2 --speed 0 "$tmp/gap.bin" 2>"$tmp/play.err" ||
  { cat "$tmp/play.err"; fail "(i) 1090 .bin: rawplay exit $?"; }
wait "$f1" "$f2" || fail "(i) a 1090 client failed"
cmp -s "$tmp/i1.out" "$tmp/gap.want" || fail "(i) 1090 client 1 differs from the 110 good frames"
cmp -s "$tmp/i2.out" "$tmp/gap.want" || fail "(i) 1090 client 2 differs from the 110 good frames"
# .csv as type 2/3 frames: a long (28 hex digits) and a short (14) message, the
# timestamp as the "@" lines carry it, signal byte 0.
printf '1700000000.000000,8D4840D6202CC371C32CE0576098\n1700000000.500000,5D4840D6202CC3\n' > "$tmp/i.csv"
python3 - "$tmp/i.csv" "$tmp/csv.want" <<'PY'
import sys
def frame(epoch_ns, hexs):
    ticks = (epoch_ns * 12 // 1000) & ((1 << 48) - 1)
    t = {14: b"2", 28: b"3"}[len(hexs)]
    body = ticks.to_bytes(6, "big") + b"\x00" + bytes.fromhex(hexs)
    return b"\x1a" + t + body.replace(b"\x1a", b"\x1a\x1a")
out = b""
for line in open(sys.argv[1]):
    ts, h = line.strip().split(",")
    sec, frac = ts.split(".")
    out += frame(int(sec) * 10**9 + int((frac + "000000000")[:9]), h)
open(sys.argv[2], "wb").write(out)
PY
python3 - "$tmp/i.csv" <<'PY'
# the file must hold one 28- and one 14-digit message
import sys
lens = [len(l.strip().split(",")[1]) for l in open(sys.argv[1])]
sys.exit(0 if lens == [28, 14] else "i.csv: digits %r" % lens)
PY
p1=$(free_port)
python3 "$tmp/fetch.py" "$p1" "$tmp/i1.out" & f1=$!
timeout $((60*slowf)) "$rawplay" --1090-listen "$p1" --speed 0 "$tmp/i.csv" 2>"$tmp/play.err" ||
  { cat "$tmp/play.err"; fail "(i) 1090 .csv: rawplay exit $?"; }
wait "$f1" || fail "(i) a 1090 csv client failed"
cmp -s "$tmp/i1.out" "$tmp/csv.want" || { od -An -tx1 "$tmp/i1.out" | head -3; od -An -tx1 "$tmp/csv.want" | head -3; fail "(i) .csv not served as Beast frames"; }
echo "ok (i) rawplay_two_clients_same_frames: UAT lines, .bin frames, .csv as Beast frames"

# (i2) The .csv -> Beast bytes are pinned to what rawplay wrote before the encoder
# moved into raw_formats (beast::encodeFrame): a short and a long message, a
# 4-digit Mode A/C line and 0x1a bytes in payload and timestamp, then the whole
# 120 s fixture (one frame per line, size and SHA-256 of the stream).
beast_sha() { # beast_sha CSV: sha256 of the Beast stream rawplay serves for it
  local p out="$tmp/pin.out"
  p=$(free_port)
  python3 "$tmp/fetch.py" "$p" "$out" & local f=$!
  timeout $((60*slowf)) "$rawplay" --1090-listen "$p" --speed 0 "$1" 2>"$tmp/play.err" ||
    { cat "$tmp/play.err"; fail "(i2) rawplay exit $?"; }
  wait "$f" || fail "(i2) pin client failed"
  echo "$(wc -c < "$out") $(sha256sum "$out" | cut -d' ' -f1)"
}
printf '1700000000.000000,8D4840D6202CC371C32CE0576098\n1700000000.500000,5D4840D6202CC3\n1700000001.000000,1A1A\n1700000001.250001,200000001a1a1a\n' > "$tmp/pin.csv"
got=$(beast_sha "$tmp/pin.csv")
[ "$got" = "71 3e7f537a73b4e6022f4fbe0b0cd03ce5adaf7e2005041d0664618b34fa6a51cd" ] || fail "(i2) pinned frames changed: $got"
if [ "$have_raw" = 1 ]; then
  zcat "$csv" > "$tmp/full.csv"
  got=$(beast_sha "$tmp/full.csv")
  [ "$got" = "3243746 10d06a99941d59dd4df2e6a9e4f625b80caf02727a59a6dc7b43a45cd6e36aaa" ] || fail "(i2) fixture stream changed: $got"
  [ "$(wc -l < "$tmp/full.csv")" = 176612 ] || fail "(i2) fixture line count"
fi
echo "ok (i2) rawplay csv -> Beast stream byte-identical to the pinned values"

# slow_client_does_not_stall_the_other: one client connects and never reads; 24 MB
# of lines at 10x the file's 24 s span. The reader gets every line on time; the
# one that does not read is dropped from, and rawplay says how much.
uat_lines 24000 1000 1 > "$tmp/j.txt"
sed 's/^[0-9]* //' "$tmp/j.txt" > "$tmp/j.want"
p1=$(free_port)
python3 "$tmp/mute.py" "$p1" & mute=$!
bg_pids+=("$mute")
python3 "$tmp/fetch.py" "$p1" "$tmp/j.out" & f1=$!
t0=$(now)
timeout $((60*slowf)) "$rawplay" --uat-listen "$p1" --wait-clients 2 --speed 10 "$tmp/j.txt" 2>"$tmp/play.err" ||
  { cat "$tmp/play.err"; fail "(i) slow client: rawplay exit $?"; }
wall=$(awk -v a="$t0" -v b="$(now)" 'BEGIN { printf "%.2f", b - a }')
wait "$f1" || fail "(i) the reading client failed"
kill "$mute" 2>/dev/null
cmp -s "$tmp/j.out" "$tmp/j.want" || fail "(i) the reading client lost lines ($(wc -l < "$tmp/j.out") of 24000)"
dropped=$(sed -n 's/.*, \([0-9]*\) bytes dropped (slow clients).*/\1/p' "$tmp/play.err")
[ -n "$dropped" ] && [ "$dropped" -gt 0 ] || { cat "$tmp/play.err"; fail "(i) nothing dropped for the client that does not read"; }
awk -v w="$wall" -v f="$slowf" 'BEGIN { exit !(w <= 2.4 + 4 * f) }' || fail "(i) 2.4 s of lines took ${wall}s: stalled"
echo "ok (i) rawplay_slow_client_does_not_stall_the_other: ${wall}s, $dropped bytes dropped for the silent client"

# wait_clients_holds_playback: with --wait-clients 2 and one client, nothing is sent;
# the second client releases it.
uat_lines 20 90 1 > "$tmp/k.txt"
sed 's/^[0-9]* //' "$tmp/k.txt" > "$tmp/k.want"
p1=$(free_port)
python3 "$tmp/fetch.py" "$p1" "$tmp/k1.out" & f1=$!
"$rawplay" --uat-listen "$p1" --wait-clients 2 --speed 0 "$tmp/k.txt" 2>"$tmp/play.err" & rp=$!
bg_pids+=("$rp")
slow_sleep 1.5
[ ! -s "$tmp/k1.out" ] || fail "(i) played with one of two clients"
kill -0 "$rp" 2>/dev/null || fail "(i) rawplay exited while waiting for the second client"
python3 "$tmp/fetch.py" "$p1" "$tmp/k2.out" & f2=$!
wait "$rp" || { cat "$tmp/play.err"; fail "(i) rawplay exit $?"; }
wait "$f1" "$f2" || fail "(i) a client failed"
cmp -s "$tmp/k1.out" "$tmp/k.want" && cmp -s "$tmp/k2.out" "$tmp/k.want" || fail "(i) the held lines differ"
echo "ok (i) rawplay_wait_clients_holds_playback"

# ---- full stack: the capture through a real readsb -----------------------------
if [ "$have_raw" = 0 ]; then
  echo "PASS (fixture sections skipped)"
  exit 0
fi
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
import os, socket, sys, time
slowf = float(os.environ.get("SKYCUE_TEST_SLOW", "1"))
deadline = time.time() + 10 * slowf
while True:
    try:
        c = socket.create_connection(("127.0.0.1", int(sys.argv[1]))); break
    except OSError:
        if time.time() > deadline: sys.exit("json: cannot connect")
        time.sleep(0.1)
c.settimeout(1)
buf = b""; n = 0; end = time.time() + 60 * slowf
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
slow_sleep 0.5
timeout $((60*slowf)) "$rawplay" --raw-in "127.0.0.1:$ri" --speed 0 "$csv" 2>"$tmp/play.err" || { cat "$tmp/play.err"; fail "full stack: rawplay exit $?"; }
wait "$reader" || { cat "$tmp/readsb.log"; fail "full stack: JSON reader failed"; }
n=$(cat "$tmp/json.count")
hexes=$(grep -o '"hex":"[^"]*"' "$tmp/json.out" | sort -u | wc -l)
[ "$n" -ge 50 ] || { tail -n 20 "$tmp/readsb.log"; fail "full stack: only $n JSON lines with lat"; }
echo "ok full stack: readsb sent $n JSON position lines for $hexes aircraft"
echo PASS
