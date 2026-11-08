#!/usr/bin/env bash
# The live differential of skycued's own UAT decoder against dump978-fa on the
# 439-downlink sample. Run (a): skycue-rawplay plays uat-sample.txt.gz once at speed 1
# (10 lines per second) to skycued (TRACK_DECODER=skycued, a uat_raw input dialling
# rawplay); skycued decodes it and records the received stream as JSONL;
# reduce_received.py --uat reduces that to one row per address. The committed golden
# (uat-sample.golden.jsonl.gz, one object per downlink) is reduced the same way by
# reduce_received.py --uat-golden (a golden position more than 300 NM from the receiver is left out: the decoder's range limit), and compare_tracks.py --uat compares the two.
# Run (b), only when SKYCUE_DUMP978 names a dump978-fa executable: gen_uat_golden.sh makes a
# fresh golden from the same sample (skycue-iqgen: the optional fourth argument) and ours is compared with that too. Two deliberately wrong copies of ours must
# fail the comparison.
# usage: test_decode_differential_uat.sh SKYCUED RAWPLAY FIXTURE_DIR [IQGEN]
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib/test_common.sh"
skycued=$1 rawplay=$2 fixtures=$3 iqgen=${4:-}
sample="$fixtures/raw/uat-sample.txt.gz"
golden="$fixtures/raw/uat-sample.golden.jsonl.gz"
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
[ -e "$sample" ] && [ -e "$golden" ] || { echo "SKIP: $sample or $golden missing"; exit 77; }
command -v python3 >/dev/null || { echo "SKIP: python3 missing"; exit 77; }
command -v curl >/dev/null || { echo "SKIP: curl missing"; exit 77; }
for f in "$skycued" "$rawplay" "$fixtures/reduce_received.py" "$fixtures/compare_tracks.py"; do
  [ -e "$f" ] || fail "$f missing"
done

# ---- run (b) in the background: a fresh dump978-fa golden ------------------------
fresh=
if [ -n "${SKYCUE_DUMP978:-}" ] && [ -x "$SKYCUE_DUMP978" ] && [ -x "$iqgen" ]; then
  fresh="$tmp/golden-fresh.jsonl.gz"
  bash "$fixtures/gen_uat_golden.sh" "$SKYCUE_DUMP978" "$iqgen" "$sample" "$fresh" >"$tmp/gen.log" 2>&1 &
  gen_pid=$!; bg_pids+=("$gen_pid")
else
  echo "run (b) skipped: SKYCUE_DUMP978 is not set to an executable (or skycue-iqgen is missing); the committed golden stands for dump978-fa"
fi

# ---- run (a): skycued decodes the sample ------------------------------------------
uat_port=$(free_port); cue_port=$(free_port); api_port=$(free_port)
ref_lat=37.4 ref_lon=-122.1
mkdir -p "$tmp/rec"
cat > "$tmp/skycue.conf" <<EOF
RECEIVER_LAT=$ref_lat
RECEIVER_LON=$ref_lon
TRACK_DECODER=skycued
CUE_TCP_PORT=$cue_port
WEB_PORT=$api_port
RECORD_DIR=$tmp/rec
RECORD_FORMATS=jsonl
EOF
cat > "$tmp/diff.inputs.json" <<EOF
{"version":2,
 "inputs":[
  {"id":"lab","name":"Lab","type":"uat_raw","transport":"tcp_connect","host":"127.0.0.1","port":$uat_port}
 ],
 "outputs":[],
 "builtin":{}}
EOF
"$rawplay" --uat-listen "$uat_port" --wait-clients 1 --speed 1 "$sample" >"$tmp/play.log" 2>&1 &
play=$!; bg_pids+=("$play")
"$skycued" --no-readsb --no-uat --config "$tmp/skycue.conf" --inputs-file "$tmp/diff.inputs.json" \
  >"$tmp/skycued.log" 2>&1 &
sk=$!; bg_pids+=("$sk")
api_up() { curl -fs -o /dev/null "http://127.0.0.1:$api_port/api/outputs"; }
wait_for 15 api_up || fail "skycued API did not come up"
t0=$(date +%s)
played() {
  grep -q 'done:' "$tmp/play.log" && return 0
  kill -0 "$play" 2>/dev/null || fail "rawplay exited before it finished"
  kill -0 "$sk" 2>/dev/null || fail "skycued died while the sample played"
  return 1
}
wait_for 200 played || fail "rawplay did not finish"
t1=$(date +%s)
echo "run (a): sample played in $((t1 - t0)) s"
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
python3 -I "$fixtures/reduce_received.py" --uat "$(rcv)" "$tmp/ours.jsonl.gz" >"$tmp/reduce.log" 2>&1 \
  || fail "reduce_received.py --uat failed"
echo "ours: $(cat "$tmp/reduce.log") (received lines $(wc -l < "$(rcv)"))"
python3 -I "$fixtures/reduce_received.py" --uat-golden "$ref_lat" "$ref_lon" "$golden" "$tmp/golden.jsonl.gz" >"$tmp/reduce.log" 2>&1 \
  || fail "reduce_received.py --uat-golden failed"
echo "committed golden: $(cat "$tmp/reduce.log")"

# ---- run (b) result -----------------------------------------------------------------
if [ -n "$fresh" ]; then
  wait_for 300 bash -c "! kill -0 $gen_pid 2>/dev/null" || fail "gen_uat_golden.sh did not finish"
  wait "$gen_pid" || fail "gen_uat_golden.sh failed"
  python3 -I "$fixtures/reduce_received.py" --uat-golden "$ref_lat" "$ref_lon" "$fresh" "$tmp/fresh.jsonl.gz" >"$tmp/reduce.log" 2>&1 \
    || fail "reduce_received.py --uat-golden failed on the fresh golden"
  echo "run (b): fresh dump978-fa golden made: $(cat "$tmp/reduce.log")"
fi

# ---- comparisons --------------------------------------------------------------------
status=0
if [ -n "$fresh" ]; then
  echo "== ours against the fresh dump978-fa golden"
  python3 -I "$fixtures/compare_tracks.py" --uat "$tmp/ours.jsonl.gz" "$tmp/fresh.jsonl.gz" \
    || { echo "FAIL: differs from fresh dump978-fa"; status=1; }
fi
echo "== ours against the committed golden"
python3 -I "$fixtures/compare_tracks.py" --uat "$tmp/ours.jsonl.gz" "$tmp/golden.jsonl.gz" \
  || { echo "FAIL: differs from committed golden"; status=1; }

# ---- negative self-tests: a wrong copy of ours must fail the comparison -------------
for kind in pos dyn; do
  python3 -I "$fixtures/compare_tracks.py" --shift "$kind" "$tmp/ours.jsonl.gz" "$tmp/shifted-$kind.jsonl.gz" \
    || fail "could not make the shifted copy ($kind)"
  if python3 -I "$fixtures/compare_tracks.py" --uat "$tmp/shifted-$kind.jsonl.gz" "$tmp/golden.jsonl.gz" >"$tmp/neg-$kind.out" 2>&1; then
    echo "FAIL: negative self-test ($kind): the shifted copy PASSED the comparison"
    status=1
  else
    echo "ok negative self-test ($kind): the shifted copy fails the comparison"
  fi
done
[ "$status" = 0 ] || exit 1
echo "PASS: decode_differential_uat"
