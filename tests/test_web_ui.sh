#!/usr/bin/env bash
# The web UI's files as skycued serves them from the staged www directory:
# /, /app.js, /app_pure.js, the Stats view's stats_pure.js, chart_theme.js
# and stats_view.js, the alert strip's alerts_pure.js and alerts_view.js, /style.css and (when Leaflet was staged)
# /vendor/leaflet/leaflet.js and (when ECharts was staged)
# /vendor/echarts/echarts.min.js with their content types, /api/state with the
# fixture's 12 tracks, /api/stats/summary with its since, a path escaping
# the www directory rejected, the
# page's scripts free of post-ES2017 syntax; then, when
# node is installed, `node --check` on the page's scripts and the pure-function
# tests (tests/js/app_pure.test.js, tests/js/stats_pure.test.js,
# tests/js/alerts_pure.test.js). Prints
# SKIP for the parts it cannot run.
# usage: test_web_ui.sh SKYCUED FIXTURE_DIR WWW_STAGE JS_TEST_DIR
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib/test_common.sh"
skycued=$1 fixtures=$2 www=$3 jsdir=$4
replay="$fixtures/readsb-sample.jsonl"
tmp=$(mktemp -d)
pid=
cleanup() {
  local i
  if [ -n "$pid" ]; then
    kill "$pid" 2>/dev/null
    for (( i=0; i<30*slowf; i++ )); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$pid" 2>/dev/null
    wait 2>/dev/null
  fi
  rm -rf "$tmp"
}
trap cleanup EXIT
fail() {
  echo "FAIL: $*"
  [ -s "$tmp/skycued.log" ] && { echo "--- skycued log:"; tail -n 20 "$tmp/skycued.log"; }
  exit 1
}
for f in "$skycued" "$replay" "$www/index.html" "$www/app.js" "$www/app_pure.js" "$www/style.css" \
  "$www/stats_pure.js" "$www/chart_theme.js" "$www/stats_view.js" "$www/alerts_pure.js" "$www/alerts_view.js"; do
  [ -e "$f" ] || fail "$f missing"
done

web_port=$(free_port)
cat > "$tmp/skycue.conf" <<EOF
RECEIVER_LAT=38.9
RECEIVER_LON=-77.0
CUE_TCP_PORT=0
WEB_PORT=$web_port
EOF
"$skycued" --replay "$replay" --replay-speed 1 --replay-loop --config "$tmp/skycue.conf" \
  --www-dir "$www" >"$tmp/skycued.log" 2>&1 &
pid=$!
base="http://127.0.0.1:$web_port"
for (( i=0; i<100*slowf; i++ )); do curl -fs -o /dev/null "$base/api/stats" && break; sleep 0.1; done
curl -fs -o /dev/null "$base/api/stats" || fail "web server did not come up"

# get PATH WANT_STATUS WANT_TYPE_PREFIX
get() {
  local out code type
  out=$(curl -s --path-as-is -o "$tmp/body" -w '%{http_code} %{content_type}' "$base$1")
  code=${out%% *} type=${out#* }
  [ "$code" = "$2" ] || fail "GET $1: status $code, want $2"
  [ -z "${3:-}" ] || [[ "$type" == "$3"* ]] || fail "GET $1: Content-Type '$type', want $3"
}
get / 200 text/html
grep -q 'vendor/leaflet/leaflet.js' "$tmp/body" || fail "index.html does not load Leaflet"
grep -q 'app_pure.js' "$tmp/body" && grep -q 'app.js' "$tmp/body" || fail "index.html does not load the scripts"
for js in vendor/echarts/echarts.min.js stats_pure.js chart_theme.js stats_view.js alerts_pure.js alerts_view.js; do
  grep -q "src=\"$js\"" "$tmp/body" || fail "index.html does not load $js"
done
get /app.js 200 application/javascript
get /app_pure.js 200 application/javascript
for js in stats_pure.js chart_theme.js stats_view.js alerts_pure.js alerts_view.js; do get "/$js" 200 application/javascript; done
get /style.css 200 text/css
if [ -e "$www/vendor/leaflet/leaflet.js" ]; then
  get /vendor/leaflet/leaflet.js 200 application/javascript
  get /vendor/leaflet/leaflet.css 200 text/css
  get /vendor/leaflet/images/layers.png 200 image/png
else
  echo "SKIP: Leaflet not staged (configure with -DSKYCUE_LEAFLET_DIR)"
fi
if [ -e "$www/vendor/echarts/echarts.min.js" ]; then
  get /vendor/echarts/echarts.min.js 200 application/javascript
  get /vendor/echarts/LICENSE 200 application/octet-stream
  get /vendor/echarts/NOTICE 200 application/octet-stream
else
  echo "SKIP: ECharts not staged (configure with -DSKYCUE_ECHARTS_DIR)"
fi
for bad in '/../etc/passwd' '/%2e%2e/etc/passwd' '/vendor/../../etc/passwd'; do
  code=$(curl -s --path-as-is -o /dev/null -w '%{http_code}' "$base$bad")
  [ "$code" = 400 ] || [ "$code" = 404 ] || fail "GET $bad: status $code, want 400 or 404"
done

# All 12 fixture aircraft are in the table within a few seconds.
n=0
for (( i=0; i<100*slowf; i++ )); do
  n=$(curl -fs "$base/api/state" | python3 -I -c 'import json,sys; print(len(json.load(sys.stdin)["tracks"]))')
  [ "$n" = 12 ] && break
  sleep 0.1
done
[ "$n" = 12 ] || fail "/api/state has $n tracks, want 12"
# The Stats view's summary, JSON with its `since` once observations are in.
get /api/stats/summary 200 application/json
python3 -I -c 'import json,sys; s=json.load(open(sys.argv[1])); assert isinstance(s["since"], str) and s["aircraft_total"] > 0' \
  "$tmp/body" || { head -c 300 "$tmp/body"; fail "/api/stats/summary has no since or no aircraft"; }

# The page's own scripts stay plain ES2017 (as app.js says), checked
# with or without node: optional chaining `?.`, `??`, `.at(`,
# private `#names` and static class fields/blocks are later syntax. (`**`
# is ES2016: fine.)
es2017_bad='\?\.|\?\?|\.at\(|\.#[A-Za-z_]|^\s*(static\s+)?#[A-Za-z_]|\bstatic\s+[A-Za-z_$][A-Za-z0-9_$]*\s*[=;]|\bstatic\s*\{'
for js in "$www"/*.js; do
  if grep -nE "$es2017_bad" "$js"; then
    fail "$(basename "$js") uses syntax newer than ES2017 (lines above)"
  fi
done

if command -v node >/dev/null 2>&1; then
  for js in app_pure.js app.js stats_pure.js chart_theme.js stats_view.js alerts_pure.js alerts_view.js; do
    node --check "$www/$js" || fail "node --check $js"
  done
  node "$jsdir/app_pure.test.js" "$www/app_pure.js" || fail "app_pure.test.js"
  node "$jsdir/stats_pure.test.js" "$www/stats_pure.js" || fail "stats_pure.test.js"
  node "$jsdir/alerts_pure.test.js" "$www/alerts_pure.js" || fail "alerts_pure.test.js"
else
  echo "SKIP: node not installed (syntax check and pure-function tests)"
fi
echo "PASS"
