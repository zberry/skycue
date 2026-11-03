#!/usr/bin/env bash
# ctest ui_playwright: the Playwright suite (ui.spec.js) in headless Chromium
# against a skycued replaying the readsb JSON fixture in real time, on
# ephemeral web/cue ports with a temporary skycue.conf, and two more for the
# FIS-B cases, each replaying a received recording with uplinks unpaced:
# fisb/received-with-uplinks.jsonl (2015: text, NOTAMs) and
# fisb/received-tg30.jsonl (2020: NEXRAD, rasters). Dev machines only:
# prints "SKIP: playwright not installed" and passes when
# node_modules/@playwright/test is missing (see README.md). One more, the
# "receivers" skycued, runs no built-in decoder inputs and one
# pingstation_json input, ps-sim, that skycue-pingsim feeds the readsb
# fixture to (with a device status every 2 s), for the Receiver tab cases.
# usage: run.sh SKYCUED FIXTURE_DIR WWW_STAGE PINGSIM [PLAYWRIGHT ARGS...]
# (extra arguments go to `playwright test`, e.g. --grep NAME).
# Every process it starts is stopped on every way out: normal end, a
# failure, or SIGINT/SIGTERM/SIGHUP (a ctest timeout, Ctrl-C).
set -u
skycued=$1 fixtures=$2 www=$3 pingsim=$4
shift 4
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
  for f in "$tmp"/*.pid; do
    p=$(cat "$f" 2>/dev/null)
    [ -n "$p" ] || continue
    kill "$p" 2>/dev/null
    kill -CONT "$p" 2>/dev/null   # the stalled-server case may have left it stopped
    for (( i=0; i<30; i++ )); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$p" 2>/dev/null
  done
  wait 2>/dev/null
  rm -rf "$tmp"
}
trap cleanup EXIT
# A signal ends the script through exit, so the EXIT trap runs for it too.
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
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

# Map tiles: one more skycued (the readsb fixture) serving a copy of the www
# directory (without its own tiles/) with generated tiles while meta.json
# claims zoom 0..6: 2/1/1 in four coloured quadrants (the fallback for the
# zoom-6 tiles around the receiver, 4 levels up) and one native zoom-6
# tile, 6/18/24 (the receiver's), transparent in its west half, which is
# outside the "spot" region's bbox, so the page draws 2/1/1 underneath it.
tiles_www="$tmp/www-tiles"
mkdir -p "$tiles_www/tiles/2/1" "$tiles_www/tiles/6/18" || fail "tiles www dir"
for f in "$www"/* "$www"/.[!.]*; do
  [ -e "$f" ] || continue
  case $(basename "$f") in tiles|overlays) continue ;; esac
  cp -r "$f" "$tiles_www/" || fail "tiles www copy"
done
# Overlays (scripts/make-overlays.sh) for the map-style cases: one border
# line and three cities around the receiver (38.9 N, 77.0 W) of ranks 1, 4
# and 7, shown from zoom 0, 5 and 7.
mkdir -p "$tiles_www/overlays" || fail "overlays www dir"
printf '%s\n' '{"files":{"admin0":{"file":"admin0.json"},"cities":{"file":"cities.json"}},"source":"test fixture"}' \
  > "$tiles_www/overlays/meta.json"
printf '%s\n' '{"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"LineString","coordinates":[[-78,38],[-77.5,39],[-76,39.5]]}}]}' \
  > "$tiles_www/overlays/admin0.json"
printf '%s\n' '{"type":"FeatureCollection","features":[' \
  '{"type":"Feature","properties":{"name":"Alphaville","rank":1,"pop":900000},"geometry":{"type":"Point","coordinates":[-77.05,38.95]}},' \
  '{"type":"Feature","properties":{"name":"Bravoton","rank":4,"pop":120000},"geometry":{"type":"Point","coordinates":[-76.8,38.75]}},' \
  '{"type":"Feature","properties":{"name":"Charlie Falls","rank":7,"pop":20000},"geometry":{"type":"Point","coordinates":[-77.2,39.05]}}]}' \
  > "$tiles_www/overlays/cities.json"
python3 -I - "$tiles_www/tiles" <<'EOP' || fail "tiles fixture PNGs"
import os, struct, sys, zlib
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
def png(path, pixel):  # 256x256 RGBA, pixel(x, y) -> (r, g, b, a)
    raw = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(256)) for y in range(256))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 256, 256, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
quad = [[(0x2e, 0x8b, 0x57, 255), (0x1e, 0x90, 0xff, 255)], [(0xff, 0xa5, 0x00, 255), (0x80, 0x00, 0x80, 255)]]
png(os.path.join(sys.argv[1], "2/1/1.png"), lambda x, y: quad[y // 128][x // 128])
png(os.path.join(sys.argv[1], "6/18/24.png"), lambda x, y: (0, 0, 0, 0) if x < 128 else (0xff, 0x00, 0xff, 255))
EOP
printf '%s\n' '{"minZoom":0,"maxZoom":6,"bbox":[-180,-85,180,85],"regions":[{"name":"coarse","bbox":[-180,-85,180,85],"minZoom":0,"maxZoom":2},{"name":"spot","bbox":[-75.9375,30,-73.125,45],"minZoom":6,"maxZoom":6}]}' \
  > "$tiles_www/tiles/meta.json"
tiles_port=$(free_port)
sed -e "s/^WEB_PORT=.*/WEB_PORT=$tiles_port/" -e "s/^CUE_TCP_PORT=.*/CUE_TCP_PORT=0/" "$tmp/skycue.conf" > "$tmp/tiles.conf"
bash -c "echo \$\$ > '$tmp/fisb-tiles.pid'; exec '$skycued' --replay '$fixtures/readsb-sample.jsonl' --replay-speed 1 --replay-loop --config '$tmp/tiles.conf' --www-dir '$tiles_www' >>'$tmp/tiles.log' 2>&1" </dev/null >/dev/null 2>&1 &
tiles_base="http://127.0.0.1:$tiles_port"
up=0
for (( i=0; i<100; i++ )); do
  python3 -I -c 'import sys, urllib.request; urllib.request.urlopen(sys.argv[1], timeout=1)' "$tiles_base/api/stats" 2>/dev/null && { up=1; break; }
  sleep 0.1
done
[ "$up" = 1 ] || fail "tiles skycued did not come up"

# Overlays without tiles: one more skycued (the readsb fixture) whose www
# has no tiles/ and an overlays/ with the border line, while meta.json lists
# a cities.json that is not there (its fetch fails with a 404).
ov_www="$tmp/www-overlays"
mkdir -p "$ov_www/overlays" || fail "overlays-only www dir"
for f in "$tiles_www"/*; do
  case $(basename "$f") in tiles|overlays) continue ;; esac
  cp -r "$f" "$ov_www/" || fail "overlays-only www copy"
done
cp "$tiles_www/overlays/meta.json" "$tiles_www/overlays/admin0.json" "$ov_www/overlays/" || fail "overlays-only fixture"
ov_port=$(free_port)
sed -e "s/^WEB_PORT=.*/WEB_PORT=$ov_port/" -e "s/^CUE_TCP_PORT=.*/CUE_TCP_PORT=0/" "$tmp/skycue.conf" > "$tmp/overlays.conf"
bash -c "echo \$\$ > '$tmp/fisb-overlays.pid'; exec '$skycued' --replay '$fixtures/readsb-sample.jsonl' --replay-speed 1 --replay-loop --config '$tmp/overlays.conf' --www-dir '$ov_www' >>'$tmp/overlays.log' 2>&1" </dev/null >/dev/null 2>&1 &
ov_base="http://127.0.0.1:$ov_port"
up=0
for (( i=0; i<100; i++ )); do
  python3 -I -c 'import sys, urllib.request; urllib.request.urlopen(sys.argv[1], timeout=1)' "$ov_base/api/stats" 2>/dev/null && { up=1; break; }
  sleep 0.1
done
[ "$up" = 1 ] || fail "overlays-only skycued did not come up"

# Receivers: skycued without readsb/dump978, its inputs file holding ps-sim
# (pingstation_json on a free UDP port), and skycue-pingsim sending to it.
rx_port=$(free_port)
ps_port=$(python3 -I -c 'import socket; s = socket.socket(type=socket.SOCK_DGRAM); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
mkdir -p "$tmp/rx-record" || fail "receivers RECORD_DIR"
sed -e "s/^WEB_PORT=.*/WEB_PORT=$rx_port/" -e "s/^CUE_TCP_PORT=.*/CUE_TCP_PORT=0/" "$tmp/skycue.conf" > "$tmp/receivers.conf"
echo "RECORD_DIR=$tmp/rx-record" >> "$tmp/receivers.conf"
printf '{"version":1,"inputs":[{"id":"ps-sim","name":"pingsim","type":"pingstation_json","bind":"127.0.0.1","port":%s}]}\n' \
  "$ps_port" > "$tmp/receivers.inputs.json"
bash -c "echo \$\$ > '$tmp/receivers.pid'; exec '$skycued' --config '$tmp/receivers.conf' --no-readsb --no-uat --inputs-file '$tmp/receivers.inputs.json' --www-dir '$www' >>'$tmp/receivers.log' 2>&1" </dev/null >/dev/null 2>&1 &
rx_base="http://127.0.0.1:$rx_port"
up=0
for (( i=0; i<100; i++ )); do
  python3 -I -c 'import sys, urllib.request; urllib.request.urlopen(sys.argv[1], timeout=1)' "$rx_base/api/stats" 2>/dev/null && { up=1; break; }
  sleep 0.1
done
[ "$up" = 1 ] || fail "receivers skycued did not come up"
bash -c "echo \$\$ > '$tmp/pingsim.pid'; exec '$pingsim' --udp '127.0.0.1:$ps_port' --as json --loop --speed 5 --status-interval 2 --status-lat 38.9 --status-lon -77.0 '$fixtures/readsb-sample.jsonl' >>'$tmp/pingsim.log' 2>&1" </dev/null >/dev/null 2>&1 &

out_dir=$(dirname "$www")/ui-test-results
rm -rf "$out_dir"
export SKYCUE_BASE_URL="$base"
export SKYCUE_CONF_PATH="$tmp/skycue.conf"
export SKYCUE_START_CMD="$start_cmd"
export SKYCUE_PID_FILE="$pidfile"
export SKYCUE_FISB_URL="$fisb_base"
export SKYCUE_RADAR_URL="$radar_base"
export SKYCUE_TILES_URL="$tiles_base"
export SKYCUE_OVERLAYS_URL="$ov_base"
export SKYCUE_RECEIVERS_URL="$rx_base"
export SKYCUE_CUE_PORT="$cue_port"
export SKYCUE_UI_OUT="$out_dir"
export PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1
# In the background with its PID kept, so a signal is acted on at once
# (bash defers its traps while a foreground child runs) and cleanup stops it.
(cd "$here" && exec npx --no-install playwright test "$@") &
echo $! > "$tmp/playwright.pid"
wait $!
rc=$?
rm -f "$tmp/playwright.pid"

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
