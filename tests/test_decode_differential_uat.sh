#!/usr/bin/env bash
# The live differential of skycued's own UAT decoder against dump978-fa, over every UAT
# capture whose files are present: the FlightAware sample (439 downlinks) and the two Stratux
# captures. Run (a): skycue-rawplay plays the capture to skycued (TRACK_DECODER=skycued, a
# uat_raw input dialling rawplay; the sample at speed 1, 10 lines per second, the Stratux
# captures at --speed 0, as fast as skycued reads them with nothing dropped; the comparison is
# per address and not time-sensitive); skycued decodes
# it and records the received stream as JSONL; reduce_received.py --uat reduces that to one
# row per address. The committed golden (one object per downlink) is reduced the same way by
# reduce_received.py --uat-golden (a golden position more than 300 NM from the receiver is left
# out, the decoder's range limit, and the number left out is asserted), and
# compare_tracks.py --uat compares the two.
# Run (b), only for the sample and only when SKYCUE_DUMP978 names a dump978-fa executable:
# gen_uat_golden.sh makes a fresh golden from the same sample (skycue-iqgen: the optional
# fourth argument) and ours is compared with that too. Two deliberately wrong copies of ours
# must fail the comparison.
# usage: test_decode_differential_uat.sh SKYCUED RAWPLAY FIXTURE_DIR [IQGEN]
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib/test_common.sh"
skycued=$1 rawplay=$2 fixtures=$3 iqgen=${4:-}
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
command -v python3 >/dev/null || { echo "SKIP: python3 missing"; exit 77; }
command -v curl >/dev/null || { echo "SKIP: curl missing"; exit 77; }
for f in "$skycued" "$rawplay" "$fixtures/reduce_received.py" "$fixtures/compare_tracks.py"; do
  [ -e "$f" ] || fail "$f missing"
done

# run_case NAME SAMPLE GOLDEN SPEED REF EXPECT_EXCLUDED FRESH
#   REF is "LAT LON", or "median" for the median position of the golden: the decoder rejects
#   a position more than 300 NM from the receiver, and the Stratux captures come from a
#   place of their own, so their receiver is put in the middle of the aircraft.
#   EXPECT_EXCLUDED is how many golden aircraft the reducer may leave out as beyond 300 NM
#   (the sample has one in the Indian Ocean). FRESH is 1 to also run dump978-fa afresh
#   (the sample only: the Stratux modulation would need about 310 MB of scratch disk).
run_case() {
  local name=$1 sample=$2 golden=$3 speed=$4 ref=$5 expect=$6 want_fresh=$7
  local d="$tmp/$name" fresh="" gen_pid="" ref_lat ref_lon
  [ -e "$sample" ] && [ -e "$golden" ] || { echo "case $name: skipped, $sample or $golden is not there"; return 0; }
  mkdir -p "$d/rec"
  if [ "$ref" = median ]; then
    local med
    med=$(python3 -I -c '
import gzip, json, statistics, sys
p = [json.loads(l) for l in gzip.open(sys.argv[1], "rt") if l.strip()]
p = [(o["lat"], o["lon"]) for o in p if "lat" in o]
if not p:
    sys.exit(3)
print(statistics.median(a for a, _ in p), statistics.median(b for _, b in p))' "$golden") \
      || fail "case $name: golden $golden has no positions, cannot take the median receiver"
    read -r ref_lat ref_lon <<<"$med"
  else
    read -r ref_lat ref_lon <<<"$ref"
  fi
  echo "== case $name: receiver $ref_lat $ref_lon, speed $speed"

  # run (b) in the background: a fresh dump978-fa golden
  if [ "$want_fresh" = 1 ]; then
    if [ -n "${SKYCUE_DUMP978:-}" ] && [ -x "$SKYCUE_DUMP978" ] && [ -x "$iqgen" ]; then
      fresh="$d/golden-fresh.jsonl.gz"
      bash "$fixtures/gen_uat_golden.sh" "$SKYCUE_DUMP978" "$iqgen" "$sample" "$fresh" >"$tmp/gen.log" 2>&1 &
      gen_pid=$!; bg_pids+=("$gen_pid")
    else
      echo "run (b) skipped: SKYCUE_DUMP978 is not set to an executable (or skycue-iqgen is missing); the committed golden stands for dump978-fa"
    fi
  fi

  # run (a): skycued decodes the sample
  local uat_port cue_port api_port
  uat_port=$(free_port); cue_port=$(free_port); api_port=$(free_port)
  cat > "$d/skycue.conf" <<EOF
RECEIVER_LAT=$ref_lat
RECEIVER_LON=$ref_lon
TRACK_DECODER=skycued
CUE_TCP_PORT=$cue_port
WEB_PORT=$api_port
RECORD_DIR=$d/rec
RECORD_FORMATS=jsonl
EOF
  cat > "$d/diff.inputs.json" <<EOF
{"version":2,
 "inputs":[
  {"id":"lab","name":"Lab","type":"uat_raw","transport":"tcp_connect","host":"127.0.0.1","port":$uat_port}
 ],
 "outputs":[],
 "builtin":{}}
EOF
  "$rawplay" --uat-listen "$uat_port" --wait-clients 1 --speed "$speed" "$sample" >"$tmp/play.log" 2>&1 &
  local play=$!; bg_pids+=("$play")
  "$skycued" --no-readsb --no-uat --config "$d/skycue.conf" --inputs-file "$d/diff.inputs.json" \
    >"$tmp/skycued.log" 2>&1 &
  local sk=$!; bg_pids+=("$sk")
  api_up() { curl -fs -o /dev/null "http://127.0.0.1:$api_port/api/outputs"; }
  wait_for 15 api_up || fail "$name: skycued API did not come up"
  local t0 t1
  t0=$(date +%s)
  played() {
    grep -q 'done:' "$tmp/play.log" && return 0
    kill -0 "$play" 2>/dev/null || fail "$name: rawplay exited before it finished"
    kill -0 "$sk" 2>/dev/null || fail "$name: skycued died while the sample played"
    return 1
  }
  wait_for 280 played || fail "$name: rawplay did not finish"
  t1=$(date +%s)
  echo "run (a): played in $((t1 - t0)) s"
  rcv() { find "$d/rec" -name 'received-*.jsonl' | head -n 1; }
  nlines() { local f; f=$(rcv); [ -n "$f" ] && wc -l < "$f" || echo 0; }
  # stable: the received line count unchanged over 2 s
  stable() { local a b; a=$(nlines); sleep 2; b=$(nlines); [ "$a" -gt 0 ] && [ "$a" = "$b" ]; }
  wait_for 30 stable || fail "$name: received recording did not settle (lines $(nlines))"
  kill -0 "$sk" 2>/dev/null || fail "$name: skycued died"
  kill "$sk" 2>/dev/null; wait "$sk" 2>/dev/null
  local keep=() p
  for p in "${bg_pids[@]}"; do [ "$p" != "$sk" ] && keep+=("$p"); done
  bg_pids=("${keep[@]}")
  [ -n "$(rcv)" ] || fail "$name: no received-*.jsonl under RECORD_DIR"
  python3 -I "$fixtures/reduce_received.py" --uat "$(rcv)" "$d/ours.jsonl.gz" >"$tmp/reduce.log" 2>&1 \
    || fail "$name: reduce_received.py --uat failed"
  echo "ours: $(cat "$tmp/reduce.log") (received lines $(wc -l < "$(rcv)"))"
  python3 -I "$fixtures/reduce_received.py" --uat-golden "$ref_lat" "$ref_lon" "$golden" "$d/golden.jsonl.gz" >"$tmp/reduce.log" 2>&1 \
    || fail "$name: reduce_received.py --uat-golden failed"
  echo "committed golden: $(cat "$tmp/reduce.log")"
  grep -q "^excluded=$expect " <(tail -n 1 "$tmp/reduce.log") \
    || fail "$name: the reducer left out a different number of golden aircraft than the expected $expect"

  # run (b) result
  if [ -n "$fresh" ]; then
    wait_for 300 bash -c "! kill -0 $gen_pid 2>/dev/null" || fail "gen_uat_golden.sh did not finish"
    wait "$gen_pid" || fail "gen_uat_golden.sh failed"
    python3 -I "$fixtures/reduce_received.py" --uat-golden "$ref_lat" "$ref_lon" "$fresh" "$d/fresh.jsonl.gz" >"$tmp/reduce.log" 2>&1 \
      || fail "$name: reduce_received.py --uat-golden failed on the fresh golden"
    echo "run (b): fresh dump978-fa golden made: $(cat "$tmp/reduce.log")"
    echo "-- $name: ours against the fresh dump978-fa golden"
    python3 -I "$fixtures/compare_tracks.py" --uat "$d/ours.jsonl.gz" "$d/fresh.jsonl.gz" \
      || { echo "FAIL: $name differs from fresh dump978-fa"; status=1; }
  fi
  echo "-- $name: ours against the committed golden"
  python3 -I "$fixtures/compare_tracks.py" --uat "$d/ours.jsonl.gz" "$d/golden.jsonl.gz" \
    || { echo "FAIL: $name differs from committed golden"; status=1; }

  # negative self-tests: a wrong copy of ours must fail the comparison
  local kind
  for kind in pos dyn; do
    python3 -I "$fixtures/compare_tracks.py" --shift "$kind" "$d/ours.jsonl.gz" "$d/shifted-$kind.jsonl.gz" \
      || fail "$name: could not make the shifted copy ($kind)"
    if python3 -I "$fixtures/compare_tracks.py" --uat "$d/shifted-$kind.jsonl.gz" "$d/golden.jsonl.gz" >"$d/neg-$kind.out" 2>&1; then
      echo "FAIL: $name negative self-test ($kind): the shifted copy PASSED the comparison"
      status=1
    else
      echo "ok $name negative self-test ($kind): the shifted copy fails the comparison"
    fi
  done
}

status=0
raw="$fixtures/raw"
run_case sample "$raw/uat-sample.txt.gz" "$raw/uat-sample.golden.jsonl.gz" 1 "37.4 -122.1" 1 1
run_case stratux "$raw/stratux-uat-2015.log.gz" "$raw/uat-stratux.golden.jsonl.gz" 0 median 0 0
run_case stratux-b "$raw/stratux-uat-2015b.log.gz" "$raw/uat-stratux-b.golden.jsonl.gz" 0 median 0 0
[ -e "$raw/uat-sample.txt.gz" ] || { echo "SKIP: the UAT sample is not there"; exit 77; }
[ "$status" = 0 ] || exit 1
echo "PASS: decode_differential_uat"
