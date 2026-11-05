#!/usr/bin/env bash
# The product tree configured on its own takes Leaflet and ECharts from SKYCUE_SOURCE_CACHE: present and
# matching -> extracted; missing or wrong hash -> configure fails with one clear line, no download.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
for f in leaflet-1.9.4.tgz echarts-5.6.0.tgz; do
  [ -f "$here/sources/$f" ] || { echo "SKIP test_skycue_web_tarballs: sources/$f missing"; exit 77; }
done
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cfg() { cmake -S "$here" -B "$tmp/b$1" -DSKYCUE_SOURCE_CACHE="$tmp/c$1" -DSKYCUE_BUILD_TESTS=OFF -DSKYCUE_BUILD_EXAMPLES=OFF 2>&1; }
# 1. both present
mkdir -p "$tmp/c1"; cp "$here"/sources/{leaflet-1.9.4,echarts-5.6.0}.tgz "$tmp/c1/"
out=$(cfg 1) || { echo "FAIL: configure with both tarballs"; echo "$out"; exit 1; }
test -f "$tmp/b1/web-tarballs/leaflet/package/dist/leaflet.js" || { echo "FAIL: leaflet not extracted"; exit 1; }
test -f "$tmp/b1/web-tarballs/echarts/package/licenses/LICENSE-d3" || { echo "FAIL: echarts not extracted"; exit 1; }
# 2. leaflet missing
mkdir -p "$tmp/c2"; cp "$here/sources/echarts-5.6.0.tgz" "$tmp/c2/"
out=$(cfg 2) && { echo "FAIL: configure succeeded without leaflet"; exit 1; }
echo "$out" | grep -q "missing source tarball $tmp/c2/leaflet-1.9.4.tgz for leaflet" || { echo "FAIL: no missing-file message"; echo "$out"; exit 1; }
echo "$out" | grep -qi "http" && { echo "FAIL: mentions a download"; exit 1; }
echo "$out" | grep -q "fill sources/ with the two web tarballs" || { echo "FAIL: missing-file hint does not say to fill sources/"; echo "$out"; exit 1; }
# 2b. skycue/ has no fetch mode: SKYCUE_FETCH_SOURCES=ON given to it changes nothing
out=$(cmake -S "$here" -B "$tmp/b2b" -DSKYCUE_SOURCE_CACHE="$tmp/c2" -DSKYCUE_FETCH_SOURCES=ON -DSKYCUE_BUILD_TESTS=OFF -DSKYCUE_BUILD_EXAMPLES=OFF 2>&1) && { echo "FAIL: configure succeeded without leaflet (SKYCUE_FETCH_SOURCES=ON)"; exit 1; }
echo "$out" | grep -q "missing source tarball $tmp/c2/leaflet-1.9.4.tgz for leaflet" || { echo "FAIL: SKYCUE_FETCH_SOURCES=ON skipped the missing-file check"; echo "$out"; exit 1; }
# 3. echarts corrupted
mkdir -p "$tmp/c3"; cp "$here/sources/leaflet-1.9.4.tgz" "$tmp/c3/"; head -c 1000 "$here/sources/echarts-5.6.0.tgz" > "$tmp/c3/echarts-5.6.0.tgz"
out=$(cfg 3) && { echo "FAIL: configure succeeded with a bad echarts"; exit 1; }
echo "$out" | grep -q "hash mismatch for $tmp/c3/echarts-5.6.0.tgz (echarts)" || { echo "FAIL: no hash message"; echo "$out"; exit 1; }
# 4. the default cache is the project's sources/, sources/ beside CMakeLists.txt
cmake -S "$here" -B "$tmp/b4" -DSKYCUE_BUILD_TESTS=OFF -DSKYCUE_BUILD_EXAMPLES=OFF >/dev/null 2>&1 || { echo "FAIL: configure with the default cache"; exit 1; }
cache=$(sed -n 's/^SKYCUE_SOURCE_CACHE:PATH=//p' "$tmp/b4/CMakeCache.txt")
[ "$cache" = "$here/sources" ] || { echo "FAIL: default SKYCUE_SOURCE_CACHE is '$cache', want $here/sources"; exit 1; }
echo PASS
