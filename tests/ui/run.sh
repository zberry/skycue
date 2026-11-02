#!/usr/bin/env bash
# ctest ui_playwright: the Playwright suite (ui.spec.js) in headless Chromium
# against a skycued replaying the readsb JSON fixture in real time, on
# ephemeral web/cue ports with a temporary skycue.conf, and two more for the
# FIS-B cases, each replaying a received recording with uplinks unpaced:
# fisb/received-with-uplinks.jsonl (2015: text, NOTAMs) and
# fisb/received-tg30.jsonl (2020: NEXRAD, rasters). Dev machines only:
# prints "SKIP: playwright not installed" and passes when
# node_modules/@playwright/test is missing (see README.md).
# usage: run.sh SKYCUED FIXTURE_DIR WWW_STAGE
set -u
skycued=$1 fixtures=$2 www=$3
here=$(cd "$(dirname "$0")" && pwd)
shots_dir=${SKYCUE_UI_SCREENSHOTS:-"$here/../../../.superpowers/sdd/2026-11-01-skycue-ui/screenshots"}
fisb_shots_dir=${SKYCUE_FISB_SCREENSHOTS:-"$here/../../../.superpowers/sdd/2026-11-02-skycue-fisb/screenshots"}

if ! command -v node >/dev/null 2>&1 || [ ! -d "$here/node_modules/@playwright/test" ]; then
  echo "SKIP: playwright not installed"
  exit 0
fi
if [ ! -e "$www/vendor/leaflet/leaflet.js" ]; then
  echo "SKIP: Leaflet not staged in $www (configure with -DSKYCUE_LEAFLET_DIR)"
  exit 0
fi

tmp=$(mktemp -d)
pidfile="$tmp/skycued.pid"
cleanup() {
  local f p i
  for f in "$pidfile" "$tmp"/fisb-*.pid; do
    p=$(cat "$f" 2>/dev/null)
    [ -n "$p" ] || continue
    kill "$p" 2>/dev/null
    for (( i=0; i<30; i++ )); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$p" 2>/dev/null
  done
  wait 2>/dev/null
  rm -rf "$tmp"
}
trap cleanup EXIT
fail() {
  echo "FAIL: $*"
  local log
  for log in "$tmp"/*.log; do
    [ -s "$log" ] && { echo "--- $(basename "$log"):"; tail -n 30 "$log"; }
  done
  exit 1
}

free_port() {
  python3 -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}
web_port=$(free_port)
cue_port=$(free_port)
cat > "$tmp/skycue.conf" <<EOF
RECEIVER_LAT=38.9
RECEIVER_LON=-77.0
CUE_TCP_PORT=$cue_port
WEB_PORT=$web_port
MAX_RANGE_NM=150
MIN_ELEVATION_DEG=0
TRAIL_SECONDS=120
EOF

# The spec restarts skycued (reconnect case) with this same command; `exec`
# keeps the PID the one of skycued itself, written to $pidfile.
start_cmd="echo \$\$ > '$pidfile'; exec '$skycued' --replay '$fixtures/readsb-sample.jsonl' --replay-speed 1 --replay-loop --config '$tmp/skycue.conf' --www-dir '$www' >>'$tmp/skycued.log' 2>&1"
bash -c "$start_cmd" &
base="http://127.0.0.1:$web_port"
up=0
for (( i=0; i<100; i++ )); do
  if python3 -I -c 'import sys, urllib.request; urllib.request.urlopen(sys.argv[1], timeout=1)' "$base/api/stats" 2>/dev/null; then
    up=1; break
  fi
  sleep 0.1
done
[ "$up" = 1 ] || fail "skycued web server did not come up"

# start_fisb NAME RECORDING LAT LON: a skycued replaying RECORDING unpaced
# (no loop: the store keeps what it decoded, on the data clock), its
# receiver at LAT/LON; prints its base URL. The data clock then runs on
# from the last record, so every product stays inside its lifetime for the
# length of the suite.
start_fisb() {
  local name=$1 rec=$2 lat=$3 lon=$4 port i
  port=$(free_port)
  cat > "$tmp/$name.conf" <<EOC
RECEIVER_LAT=$lat
RECEIVER_LON=$lon
CUE_TCP_PORT=0
WEB_PORT=$port
MAX_RANGE_NM=150
MIN_ELEVATION_DEG=0
EOC
  bash -c "echo \$\$ > '$tmp/$name.pid'; exec '$skycued' --replay '$rec' --replay-speed 0 --config '$tmp/$name.conf' --www-dir '$www' >>'$tmp/$name.log' 2>&1" </dev/null >/dev/null 2>&1 &
  for (( i=0; i<100; i++ )); do
    python3 -I -c 'import sys, urllib.request; urllib.request.urlopen(sys.argv[1], timeout=1)' \
      "http://127.0.0.1:$port/api/stats" 2>/dev/null && { echo "http://127.0.0.1:$port"; return 0; }
    sleep 0.1
  done
  return 1
}
# wait_fisb BASE UPLINKS: until BASE's FIS-B store has decoded UPLINKS
# uplinks and published its NEXRAD (published 10 s after the last block).
wait_fisb() {
  python3 -I - "$1" "$2" <<'EOP'
import json, sys, time, urllib.request
base, want = sys.argv[1], int(sys.argv[2])
for _ in range(300):
    try:
        s = json.load(urllib.request.urlopen(base + "/api/fisb/status", timeout=1))
        if s.get("uplinks", 0) >= want and all("published" in r for r in s.get("radar", {}).values()):
            sys.exit(0)
    except Exception:
        pass
    time.sleep(0.1)
sys.exit(1)
EOP
}
# The 2015 sample's one ground station (San Jose area); tg30's (central
# Indiana), inside both of its NEXRAD mosaics (regional 36.7-43.3 N,
# 90.4-81.6 W; CONUS 20-53 N, 132-60 W).
fisb_rec="$fixtures/fisb/received-with-uplinks.jsonl"
radar_rec="$fixtures/fisb/received-tg30.jsonl"
fisb_base=$(start_fisb fisb-2015 "$fisb_rec" 37.32 -121.75) || fail "FIS-B skycued (2015) did not come up"
radar_base=$(start_fisb fisb-tg30 "$radar_rec" 40.04 -86.26) || fail "FIS-B skycued (tg30) did not come up"
wait_fisb "$fisb_base" "$(grep -c '"uat_uplink"' "$fisb_rec")" || fail "2015 uplinks not decoded"
wait_fisb "$radar_base" "$(grep -c '"uat_uplink"' "$radar_rec")" || fail "tg30 uplinks not decoded"

out_dir=$(dirname "$www")/ui-test-results
rm -rf "$out_dir"
export SKYCUE_BASE_URL="$base"
export SKYCUE_CONF_PATH="$tmp/skycue.conf"
export SKYCUE_START_CMD="$start_cmd"
export SKYCUE_PID_FILE="$pidfile"
export SKYCUE_FISB_URL="$fisb_base"
export SKYCUE_RADAR_URL="$radar_base"
export SKYCUE_UI_OUT="$out_dir"
export PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1
(cd "$here" && npx --no-install playwright test)
rc=$?

# copy_shots PATTERN DIR
copy_shots() {
  local f n=0
  mkdir -p "$2"
  while IFS= read -r -d '' f; do
    cp "$f" "$2/" && n=$((n + 1))
  done < <(find "$out_dir" -name "$1" -print0 2>/dev/null)
  echo "copied $n screenshots to $2"
}
copy_shots 'skycue-*.png' "$shots_dir"
copy_shots 'fisb-*.png' "$fisb_shots_dir"
[ "$rc" -eq 0 ] || fail "playwright exit $rc"
echo "PASS"
