#!/usr/bin/env bash
# Soak: two skycued instances under load, sampled until the end, then judged
# by soak_eval.py (RSS flat over the last 75 %, fds flat). docs/hardening.md
# "Soak" has the procedure and the recorded runs.
#
# usage: soak.sh (--stage DIR | --skycued BIN --pingsim BIN --dump BIN --www DIR)
#                [--minutes N] [--interval S] [--out DIR] [--fixtures DIR]
#                [--growth F] [--fd-tolerance N]
#
#   --stage DIR     an installed prefix (DIR/bin/skycued, DIR/bin/skycue-pingsim,
#                   DIR/bin/skycue-dump, DIR/share/skycue/www): the tree's own
#                   `cmake --install` stage, never another checkout's
#   --minutes N     run length (default 240)
#   --interval S    seconds between samples (default 60)
#   --out DIR       where A.pid, B.pid, A.log, B.log, soak.csv and verdict.txt
#                   go (default ./soak-<UTC timestamp>)
#   --fixtures DIR  skycue/tests/fixtures (default: next to this script)
#   --growth F      largest RSS growth over the window (default 0.05)
#   --fd-tolerance N  largest fd count change over the window (default 2)
#
# Instance A replays fixtures/fisb/received-tg30.jsonl in a loop (FIS-B
# uplinks only: its observations and tracks stay 0); instance B has no
# readsb/dump978 and one pingstation_json input fed by skycue-pingsim (the
# tracks). Each has a WebSocket client (ws_client.mjs) and a cue TCP
# client (skycue-dump --tcp on the port the daemon logged). Ports are
# ephemeral; every wait is a bounded poll; everything started is stopped by
# its recorded pid on every exit path.
#
# Exit 0 PASS, 1 FAIL, 77 SKIP (no node, a node without WebSocket, or no
# curl), 2 usage.

set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

usage() {
  sed -n '6,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 2
}

stage="" skycued="" pingsim="" dump="" www=""
minutes=240 interval=60 out="" fixtures="$here/../fixtures"
growth=0.05 fd_tolerance=2
while [ $# -gt 0 ]; do
  case $1 in
    --stage) stage=${2-}; shift 2 || usage ;;
    --skycued) skycued=${2-}; shift 2 || usage ;;
    --pingsim) pingsim=${2-}; shift 2 || usage ;;
    --dump) dump=${2-}; shift 2 || usage ;;
    --www) www=${2-}; shift 2 || usage ;;
    --minutes) minutes=${2-}; shift 2 || usage ;;
    --interval) interval=${2-}; shift 2 || usage ;;
    --out) out=${2-}; shift 2 || usage ;;
    --fixtures) fixtures=${2-}; shift 2 || usage ;;
    --growth) growth=${2-}; shift 2 || usage ;;
    --fd-tolerance) fd_tolerance=${2-}; shift 2 || usage ;;
    -h|--help) usage ;;
    *) echo "soak.sh: unknown argument $1" >&2; usage ;;
  esac
done

if [ -n "$stage" ]; then
  skycued=${skycued:-$stage/bin/skycued}
  pingsim=${pingsim:-$stage/bin/skycue-pingsim}
  dump=${dump:-$stage/bin/skycue-dump}
  www=${www:-$stage/share/skycue/www}
fi
[ -n "$skycued" ] && [ -n "$pingsim" ] && [ -n "$dump" ] && [ -n "$www" ] || {
  echo "soak.sh: give --stage, or --skycued, --pingsim, --dump and --www" >&2; usage; }
for b in "$skycued" "$pingsim" "$dump"; do
  [ -x "$b" ] || { echo "soak.sh: $b is not an executable" >&2; exit 2; }
done
[ -d "$www" ] || { echo "soak.sh: www dir $www not found" >&2; exit 2; }
case $minutes in ''|*[!0-9]*) echo "soak.sh: --minutes must be a whole number" >&2; exit 2 ;; esac
case $interval in ''|*[!0-9]*|0) echo "soak.sh: --interval must be a whole number of seconds, at least 1" >&2; exit 2 ;; esac
[ "$minutes" -ge 1 ] || { echo "soak.sh: --minutes must be at least 1" >&2; exit 2; }
fixtures=$(cd "$fixtures" 2>/dev/null && pwd) || { echo "soak.sh: fixtures dir not found" >&2; exit 2; }
fisb_rec="$fixtures/fisb/received-tg30.jsonl"
readsb_rec="$fixtures/readsb-sample.jsonl"
for f in "$fisb_rec" "$readsb_rec"; do
  [ -r "$f" ] || { echo "soak.sh: fixture $f not found" >&2; exit 2; }
done
python3 -I -c 'import sys; float(sys.argv[1]); int(sys.argv[2])' "$growth" "$fd_tolerance" 2>/dev/null || {
  echo "soak.sh: --growth must be a number and --fd-tolerance a whole number" >&2; exit 2; }

command -v node >/dev/null 2>&1 || { echo "SKIP: node not found"; exit 77; }
# ws_client.mjs uses node's built-in WebSocket (node 22 and later).
node -e 'process.exit(typeof WebSocket==="function"?0:1)' >/dev/null 2>&1 || {
  echo "SKIP: node without WebSocket (need node >= 22)"; exit 77; }
command -v curl >/dev/null 2>&1 || { echo "SKIP: curl not found"; exit 77; }

slow=${SKYCUE_TEST_SLOW:-1}
case $slow in ''|*[!0-9]*|0) slow=1 ;; esac

out=${out:-./soak-$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$out" || { echo "soak.sh: cannot create $out" >&2; exit 2; }
out=$(cd "$out" && pwd)
rm -f "$out"/*.pid "$out"/A.log "$out"/B.log "$out"/soak.csv "$out"/verdict.txt "$out"/*.client.log
work=$(mktemp -d "$out/work.XXXXXX") || { echo "soak.sh: mktemp failed" >&2; exit 2; }

# --- process bookkeeping ----------------------------------------------------
# names[i] and pids[i]: everything started, in start order; stopped in reverse.
names=() pids=()

record() {  # record NAME PID: remember it and write OUT/NAME.pid
  names+=("$1"); pids+=("$2")
  echo "$2" > "$out/$1.pid"
}

# alive PID: running (an exited child stays a zombie until reaped; that is
# not alive).
alive() {
  local st
  [ -r "/proc/$1/stat" ] || return 1
  st=$(cut -d' ' -f3 "/proc/$1/stat" 2>/dev/null) || return 1
  [ -n "$st" ] && [ "$st" != Z ] && [ "$st" != X ]
}

stopped=0
stop_all() {
  [ "$stopped" = 1 ] && return
  stopped=1
  local i p n
  for (( i=${#pids[@]}-1; i>=0; i-- )); do
    alive "${pids[$i]}" && kill -TERM "${pids[$i]}" 2>/dev/null
  done
  # Up to 3 s for all of them, then KILL what is left.
  for (( n=0; n<30; n++ )); do
    p=0
    for i in "${pids[@]}"; do alive "$i" && p=1; done
    [ "$p" = 0 ] && break
    sleep 0.1
  done
  for i in "${pids[@]}"; do
    alive "$i" && { echo "soak.sh: pid $i did not stop on TERM; KILL" >&2; kill -KILL "$i" 2>/dev/null; }
  done
  for i in "${pids[@]}"; do wait "$i" 2>/dev/null; done
  rm -rf "$work"
}
on_signal() {
  echo "soak.sh: $1; stopping" >&2
  stop_all
  exit 1
}
trap stop_all EXIT
trap 'on_signal SIGINT' INT
trap 'on_signal SIGTERM' TERM
trap 'on_signal SIGHUP' HUP

fail() {  # fail MESSAGE [INSTANCE]: the message, the instance's log tail, exit 1
  echo "FAIL: $1" | tee -a "$out/verdict.txt"
  if [ -n "${2-}" ] && [ -f "$out/$2.log" ]; then
    echo "--- last 20 lines of $2.log"
    tail -n 20 "$out/$2.log"
  fi
  exit 1
}

# Ports the demo uses; an ephemeral port is never one of them, but say so.
demo_port() {
  case $1 in 8080|30001|30004|30005|30047|30500|30978|30979) return 0 ;; esac
  return 1
}
free_port() {  # free_port tcp|udp
  local p n
  for (( n=0; n<20; n++ )); do
    p=$(python3 -I -c 'import socket, sys
t = socket.SOCK_DGRAM if sys.argv[1] == "udp" else socket.SOCK_STREAM
s = socket.socket(type=t); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])' "$1") || return 1
    demo_port "$p" || { echo "$p"; return 0; }
  done
  return 1
}

# wait_port INSTANCE WHAT PATTERN: sets $port to the digits that follow
# PATTERN in the first matching log line; a bounded poll (30 s x slow), and
# an empty or non-numeric port is a FAIL before any client starts.
wait_port() {
  local inst=$1 what=$2 pat=$3 log="$out/$1.log" n limit=$(( 300 * slow ))
  port=""
  for (( n=0; n<limit; n++ )); do
    port=$(grep -o -m1 -- "$pat[0-9]*" "$log" 2>/dev/null | head -n1)
    port=${port#"$pat"}
    [ -n "$port" ] && break
    alive "$(cat "$out/$inst.pid")" || break
    sleep 0.1
  done
  case $port in
    ''|*[!0-9]*) fail "cannot read the $what port of $inst" "$inst" ;;
  esac
}

# --- the instances -----------------------------------------------------------
conf() {  # conf FILE WEB_PORT CUE_PORT
  cat > "$1" <<EOF
RECEIVER_LAT=38.9
RECEIVER_LON=-77.0
WEB_PORT=$2
CUE_TCP_PORT=$3
MAX_RANGE_NM=150
MIN_ELEVATION_DEG=0
TRAIL_SECONDS=120
EOF
}

# WEB_PORT=0 and CUE_TCP_PORT=0 turn the listeners off, so each gets a free
# ephemeral port here; the daemon's own log line is still what the clients use.
for inst in A B; do
  wp=$(free_port tcp) && cp=$(free_port tcp) || fail "no free port for $inst"
  mkdir -p "$work/$inst" || fail "work dir for $inst"
  conf "$work/$inst/skycue.conf" "$wp" "$cp"
done

ps_port=$(free_port udp) || fail "no free UDP port for the pingstation input"
printf '{"version":1,"inputs":[{"id":"ps-sim","name":"pingsim","type":"pingstation_json","bind":"127.0.0.1","port":%s}]}\n' \
  "$ps_port" > "$work/B/inputs.json"

"$skycued" --replay "$fisb_rec" --replay-loop --config "$work/A/skycue.conf" --www-dir "$www" \
  </dev/null >"$out/A.log" 2>&1 &
record A $!
"$skycued" --no-readsb --no-uat --inputs-file "$work/B/inputs.json" --config "$work/B/skycue.conf" \
  --www-dir "$www" </dev/null >"$out/B.log" 2>&1 &
record B $!

declare -A web cue
for inst in A B; do
  wait_port "$inst" web 'web: listening on http://127.0.0.1:'; web[$inst]=$port
  wait_port "$inst" cue 'forwarder: listening on TCP port '; cue[$inst]=$port
  for p in "${web[$inst]}" "${cue[$inst]}"; do
    demo_port "$p" && fail "$inst listens on the demo's port $p"
  done
  echo "soak.sh: $inst pid $(cat "$out/$inst.pid"), web ${web[$inst]}, cue ${cue[$inst]}"
done

# --- the clients -------------------------------------------------------------
"$pingsim" --udp "127.0.0.1:$ps_port" --as json --loop --speed 5 --status-interval 2 \
  --status-lat 38.9 --status-lon -77.0 "$readsb_rec" </dev/null >"$out/pingsim.client.log" 2>&1 &
record pingsim $!
for inst in A B; do
  node "$here/ws_client.mjs" "ws://127.0.0.1:${web[$inst]}/ws" </dev/null >/dev/null 2>"$out/$inst-ws.client.log" &
  record "$inst-ws" $!
  "$dump" --tcp "127.0.0.1:${cue[$inst]}" </dev/null >/dev/null 2>"$out/$inst-dump.client.log" &
  record "$inst-dump" $!
done

# --- sampling ----------------------------------------------------------------
csv="$out/soak.csv"
echo "t_s,instance,pid,rss_kb,vsz_kb,fds,threads,observations,web_clients,tcp_clients,tracks_total" > "$csv"

check_alive() {  # every recorded process must still run
  local i n p s
  for (( i=0; i<${#pids[@]}; i++ )); do
    n=${names[$i]} p=${pids[$i]}
    alive "$p" && continue
    wait "$p" 2>/dev/null; s=$?
    case $n in
      A|B) fail "skycued $n exited (status $s)" "$n" ;;
      *) echo "--- $n.client.log"; tail -n 20 "$out/$n.client.log" 2>/dev/null
         fail "$n client exited (status $s)" ;;
    esac
  done
}

sample() {  # sample T INSTANCE
  local t=$1 inst=$2 pid rss vsz thr fds stats
  pid=$(cat "$out/$inst.pid")
  rss=$(awk '/^VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null)
  vsz=$(awk '/^VmSize:/ {print $2}' "/proc/$pid/status" 2>/dev/null)
  thr=$(awk '/^Threads:/ {print $2}' "/proc/$pid/status" 2>/dev/null)
  fds=$(ls -1 "/proc/$pid/fd" 2>/dev/null | wc -l)
  [ -n "$rss" ] && [ -n "$vsz" ] && [ -n "$thr" ] || return 1
  stats=$(curl -fs --max-time 5 "http://127.0.0.1:${web[$inst]}/api/stats" | python3 -I -c '
import json, sys
try:
    s = json.load(sys.stdin)
    print(",".join(str(int(s[k])) for k in ("observations", "web_clients", "tcp_clients", "tracks_total")))
except Exception:
    print(",,,")')
  [ -n "$stats" ] || stats=",,,"
  echo "$t,$inst,$pid,$rss,$vsz,$fds,$thr,$stats" >> "$csv"
}

start=$(date +%s)
duration=$(( minutes * 60 ))
next=0
echo "soak.sh: sampling every ${interval}s for ${minutes} min into $csv"
while :; do
  check_alive
  now=$(( $(date +%s) - start ))
  sample "$now" A || check_alive
  sample "$now" B || check_alive
  [ "$now" -ge "$duration" ] && break
  next=$(( next + interval ))
  [ "$next" -gt "$duration" ] && next=$duration
  # Until the next sample: a poll on the processes (0.2 s), not a fixed wait.
  while [ $(( $(date +%s) - start )) -lt "$next" ]; do
    check_alive
    sleep 0.2
  done
done

# --- verdict -----------------------------------------------------------------
# A's load is FIS-B (its observations and tracks columns stay 0): its uplink
# count, for the record.
echo "soak.sh: A FIS-B: $(curl -fs --max-time 5 "http://127.0.0.1:${web[A]}/api/fisb/status" | python3 -I -c '
import json, sys
try:
    s = json.load(sys.stdin)
    print("%s uplinks, %s products, %s bad frames" % (s.get("uplinks"), s.get("products"), s.get("bad_frames")))
except Exception:
    print("status unavailable")')"
stop_all
for inst in A B; do
  echo "soak.sh: $inst ws client: $(tail -n 1 "$out/$inst-ws.client.log" 2>/dev/null)"
  echo "soak.sh: $inst ws reconnects: $(grep -c 'reconnected' "$out/$inst-ws.client.log" 2>/dev/null)"
done
python3 -I "$here/soak_eval.py" "$csv" --growth "$growth" --fd-tolerance "$fd_tolerance" > "$out/verdict.txt"
rc=$?
cat "$out/verdict.txt"
[ "$rc" = 0 ] && echo "PASS" || echo "FAIL"
exit "$rc"
