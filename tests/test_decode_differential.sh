#!/usr/bin/env bash
# The live differential of skycued's own decoder against readsb on the 120 s Mode S
# capture. Run (a): skycue-rawplay plays the capture once at speed 1 as Beast frames;
# skycued (TRACK_DECODER=skycued, a beast_raw input; the config requires a receiver position, which becomes the decoder's reference) decodes
# it and records the received stream as JSONL; reduce_received.py reduces that to
# the shape of the golden. Run (b), only when SKYCUE_READSB names a readsb
# executable: gen_raw_golden.sh makes a fresh golden from the same capture. Ours is
# compared (compare_tracks.py) with the fresh golden when there is one and always
# with the committed modes-120s.golden.jsonl.gz. Two deliberately wrong copies of
# ours must fail the comparison.
# usage: test_decode_differential.sh SKYCUED RAWPLAY FIXTURE_DIR
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib/test_common.sh"
skycued=$1 rawplay=$2 fixtures=$3
csv="$fixtures/raw/modes-120s.csv.gz"
golden="$fixtures/raw/modes-120s.golden.jsonl.gz"
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
  for f in skycued.log play.log gen.log reduce.log; do
    [ -s "$tmp/$f" ] && { echo "--- $f:"; tail -n 15 "$tmp/$f"; }
  done
  exit 1
}
[ -e "$csv" ] || { echo "SKIP: $csv not fetched: run scripts/fixtures/fetch-test-captures.sh"; exit 77; }
command -v python3 >/dev/null || { echo "SKIP: python3 missing"; exit 77; }
command -v curl >/dev/null || { echo "SKIP: curl missing"; exit 77; }
for f in "$skycued" "$rawplay" "$golden" "$fixtures/reduce_received.py" "$fixtures/compare_tracks.py"; do
  [ -e "$f" ] || fail "$f missing"
done

# ---- run (b) in the background: a fresh readsb golden --------------------------
fresh=
if [ -n "${SKYCUE_READSB:-}" ] && [ -x "$SKYCUE_READSB" ]; then
  fresh="$tmp/golden-fresh.jsonl.gz"
  bash "$fixtures/gen_raw_golden.sh" "$SKYCUE_READSB" "$rawplay" "$csv" "$fresh" >"$tmp/gen.log" 2>&1 &
  gen_pid=$!; bg_pids+=("$gen_pid")
else
  echo "run (b) skipped: SKYCUE_READSB is not set to an executable; the committed golden stands for readsb"
fi

# ---- run (a): skycued decodes the capture --------------------------------------
raw_port=$(free_port); cue_port=$(free_port); api_port=$(free_port)
mkdir -p "$tmp/rec"
cat > "$tmp/skycue.conf" <<EOF
RECEIVER_LAT=51.7
RECEIVER_LON=4.1
TRACK_DECODER=skycued
CUE_TCP_PORT=$cue_port
WEB_PORT=$api_port
RECORD_DIR=$tmp/rec
RECORD_FORMATS=jsonl
EOF
cat > "$tmp/diff.inputs.json" <<EOF
{"version":2,
 "inputs":[
  {"id":"lab","name":"Lab","type":"beast_raw","transport":"tcp_connect","host":"127.0.0.1","port":$raw_port}
 ],
 "outputs":[],
 "builtin":{}}
EOF
"$rawplay" --1090-listen "$raw_port" --wait-clients 1 --speed 1 "$csv" >"$tmp/play.log" 2>&1 &
play=$!; bg_pids+=("$play")
"$skycued" --no-readsb --no-uat --config "$tmp/skycue.conf" --inputs-file "$tmp/diff.inputs.json" \
  >"$tmp/skycued.log" 2>&1 &
sk=$!; bg_pids+=("$sk")
api_up() { curl -fs -o /dev/null "http://127.0.0.1:$api_port/api/outputs"; }
wait_for 15 api_up || fail "skycued API did not come up"
t0=$(date +%s)
# rawplay finished, or one of the two died first (checked on every poll)
played() {
  grep -q 'done:' "$tmp/play.log" && return 0
  kill -0 "$play" 2>/dev/null || fail "rawplay exited before it finished"
  kill -0 "$sk" 2>/dev/null || fail "skycued died while the capture played"
  return 1
}
wait_for 200 played || fail "rawplay did not finish"
t1=$(date +%s)
echo "run (a): capture played in $((t1 - t0)) s"
rcv() { find "$tmp/rec" -name 'received-*.jsonl' | head -n 1; }
nlines() { local f; f=$(rcv); [ -n "$f" ] && wc -l < "$f" || echo 0; }
# stable: the received line count unchanged over 2 s
stable() { local a b; a=$(nlines); sleep 2; b=$(nlines); [ "$a" -gt 0 ] && [ "$a" = "$b" ]; }
wait_for 30 stable || fail "received recording did not settle (lines $(nlines))"
kill -0 "$sk" 2>/dev/null || fail "skycued died"
kill "$sk" 2>/dev/null; wait "$sk" 2>/dev/null
keep=()
for p in "${bg_pids[@]}"; do [ "$p" != "$sk" ] && keep+=("$p"); done
bg_pids=("${keep[@]}")
[ -n "$(rcv)" ] || fail "no received-*.jsonl under RECORD_DIR"
python3 -I "$fixtures/reduce_received.py" "$(rcv)" "$tmp/ours.jsonl.gz" >"$tmp/reduce.log" 2>&1 \
  || fail "reduce_received.py failed"
echo "ours: $(cat "$tmp/reduce.log") (received lines $(wc -l < "$(rcv)"))"

# ---- run (b) result -------------------------------------------------------------
if [ -n "$fresh" ]; then
  wait_for 300 bash -c "! kill -0 $gen_pid 2>/dev/null" || fail "gen_raw_golden.sh did not finish"
  wait "$gen_pid" || fail "gen_raw_golden.sh failed"
  echo "run (b): fresh readsb golden made"
fi

# ---- comparisons ----------------------------------------------------------------
status=0
if [ -n "$fresh" ]; then
  echo "== ours against the fresh readsb golden"
  python3 -I "$fixtures/compare_tracks.py" "$tmp/ours.jsonl.gz" "$fresh" || { echo "FAIL: differs from fresh readsb"; status=1; }
fi
echo "== ours against the committed golden"
python3 -I "$fixtures/compare_tracks.py" "$tmp/ours.jsonl.gz" "$golden" || { echo "FAIL: differs from committed golden"; status=1; }

# ---- negative self-tests: a wrong copy of ours must fail the comparison ---------
ref=${fresh:-$golden}
for kind in pos dyn; do
  python3 -I "$fixtures/compare_tracks.py" --shift "$kind" "$tmp/ours.jsonl.gz" "$tmp/shifted-$kind.jsonl.gz" \
    || fail "could not make the shifted copy ($kind)"
  if python3 -I "$fixtures/compare_tracks.py" "$tmp/shifted-$kind.jsonl.gz" "$ref" >"$tmp/neg-$kind.out" 2>&1; then
    echo "FAIL: negative self-test ($kind): the shifted copy PASSED the comparison"
    status=1
  else
    echo "ok negative self-test ($kind): the shifted copy fails the comparison"
  fi
done
[ "$status" = 0 ] || exit 1
echo "PASS: decode_differential"
