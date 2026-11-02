#!/usr/bin/env bash
# ctest ui_playwright: the Playwright suite (ui.spec.js) in headless Chromium
# against a skycued replaying the readsb JSON fixture in real time, on
# ephemeral web/cue ports with a temporary skycue.conf. Dev machines only:
# prints "SKIP: playwright not installed" and passes when
# node_modules/@playwright/test is missing (see README.md).
# usage: run.sh SKYCUED FIXTURE_DIR WWW_STAGE
set -u
skycued=$1 fixtures=$2 www=$3
here=$(cd "$(dirname "$0")" && pwd)
shots_dir=${SKYCUE_UI_SCREENSHOTS:-"$here/../../../.superpowers/sdd/2026-11-01-skycue-ui/screenshots"}

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
  local p i
  p=$(cat "$pidfile" 2>/dev/null)
  if [ -n "$p" ]; then
    kill "$p" 2>/dev/null
    for (( i=0; i<30; i++ )); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$p" 2>/dev/null
  fi
  wait 2>/dev/null
  rm -rf "$tmp"
}
trap cleanup EXIT
fail() {
  echo "FAIL: $*"
  [ -s "$tmp/skycued.log" ] && { echo "--- skycued log:"; tail -n 30 "$tmp/skycued.log"; }
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

out_dir=$(dirname "$www")/ui-test-results
rm -rf "$out_dir"
export SKYCUE_BASE_URL="$base"
export SKYCUE_CONF_PATH="$tmp/skycue.conf"
export SKYCUE_START_CMD="$start_cmd"
export SKYCUE_PID_FILE="$pidfile"
export SKYCUE_UI_OUT="$out_dir"
export PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1
(cd "$here" && npx --no-install playwright test)
rc=$?

mkdir -p "$shots_dir"
n=0
while IFS= read -r -d '' f; do
  cp "$f" "$shots_dir/" && n=$((n + 1))
done < <(find "$out_dir" -name 'skycue-*.png' -print0 2>/dev/null)
echo "copied $n screenshots to $shots_dir"
[ "$rc" -eq 0 ] || fail "playwright exit $rc"
echo "PASS"
