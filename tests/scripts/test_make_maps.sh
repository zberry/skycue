#!/usr/bin/env bash
# skycue-make-maps against stand-in make-tiles.sh and make-overlays.sh that
# only log their arguments: which regions it bakes (the whole-world base
# first, only when meta.json lacks it, then the requested region), where it
# reads MAP_DIR and the receiver position, the GDAL and container routes, the
# map-dir check and the usage errors. Bakes nothing and needs no network.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
src="$here/scripts/maps/skycue-make-maps"
tmp=$(readlink -f "$(mktemp -d)")
trap 'chmod -R u+w "$tmp" 2>/dev/null; rm -rf "$tmp"' EXIT
fail() {
  echo "FAIL: test_make_maps: $*"
  for f in out err calls; do [ -s "$tmp/$f" ] && { echo "--- $f"; cat "$tmp/$f"; }; done
  exit 1
}
[ -f "$src" ] || fail "scripts/maps/skycue-make-maps missing"
bash -n "$src" || fail "bash -n"
bash_bin=$(command -v bash)
image=ghcr.io/osgeo/gdal:ubuntu-small-3.9.3

# The front script beside its stand-in helpers, as in the source tree.
bin=$tmp/bin
mkdir -p "$bin/lib" "$tmp/tools" "$tmp/gdal" "$tmp/engine"
cp "$src" "$bin/" && cp "$here/scripts/lib/skycue-common.sh" "$bin/lib/" || fail "cannot copy the scripts"
for s in make-tiles.sh make-overlays.sh; do
  cat > "$bin/$s" <<EOF
#!/bin/sh
{ printf '%s' $s; for a in "\$@"; do printf ' %s' "\$a"; done; printf '\n'; } >> "\$STUB_LOG"
exit \${STUB_RC:-0}
EOF
  chmod +x "$bin/$s"
done
# A PATH holding only the basic tools (no GDAL or container engine from this
# machine can leak in), plus fake GDAL tools or a fake podman when wanted.
for t in awk basename cat chown dirname id mkdir readlink; do
  p=$(command -v "$t") || fail "no $t on this machine"
  ln -s "$p" "$tmp/tools/$t"
done
have_py=0
if p=$(command -v python3); then ln -s "$p" "$tmp/tools/python3"; have_py=1; fi
for t in gdal2tiles.py gdal_translate ogr2ogr; do
  printf '#!/bin/sh\nexit 1\n' > "$tmp/gdal/$t"; chmod +x "$tmp/gdal/$t"
done
printf '#!/bin/sh\nexit 1\n' > "$tmp/engine/podman"; chmod +x "$tmp/engine/podman"
GDAL=$tmp/gdal:$tmp/tools
BARE=$tmp/tools

maps=$tmp/maps
conf=$tmp/skycue.conf
printf '%s\n' '# test config' "MAP_DIR=$maps" 'RECEIVER_LAT=38.9' 'RECEIVER_LON=-77.0' > "$conf"
if [ "$(id -u)" = 0 ]; then cache=/var/cache/skycue; else cache=$tmp/xdg/skycue; fi

run() { # run PATH ARGS...: exit status in $rc; stdout, stderr, stub calls in $tmp/{out,err,calls}
  local p=$1; shift
  : > "$tmp/calls"
  PATH=$p STUB_LOG=$tmp/calls XDG_CACHE_HOME=$tmp/xdg "$bash_bin" "$bin/skycue-make-maps" "$@" \
    >"$tmp/out" 2>"$tmp/err"
  rc=$?
}
ncalls() { grep -c "^$1 " "$tmp/calls"; }
call() { grep "^$1 " "$tmp/calls" | sed -n "${2}p"; } # call STUB N: its Nth argv line
global_line="make-tiles.sh -180,-85,180,85 0 5 --name global --out $maps/tiles --cache $cache"
overlays_line="make-overlays.sh --out $maps/overlays --cache $cache"
# inside BBOX LAT LON: the point is strictly inside the bbox
inside() { awk -v b="$1" -v la="$2" -v lo="$3" 'BEGIN { split(b, a, ",");
  exit !(a[1] < lo && lo < a[3] && a[2] < la && la < a[4]) }'; }

# 1. A preset on a fresh map dir: the base region, then the preset at finer
# zooms, then the overlays, all with the dirs given explicitly.
run "$GDAL" --config "$conf" --preset conus --dry-run
[ $rc -eq 0 ] || fail "preset conus: exit $rc"
[ "$(ncalls make-tiles.sh)" = 2 ] || fail "preset conus: want 2 make-tiles.sh calls"
[ "$(ncalls make-overlays.sh)" = 1 ] || fail "preset conus: want 1 make-overlays.sh call"
[ "$(sed -n 1p "$tmp/calls")" = "$global_line --dry-run" ] || fail "preset conus: first call is not the global base"
read -r _ bbox minz maxz rest <<<"$(call make-tiles.sh 2)"
[ "$rest" = "--name conus --out $maps/tiles --cache $cache --dry-run" ] || fail "preset conus: second call: '$rest'"
[ "$minz" -ge 6 ] && [ "$maxz" -ge "$minz" ] || fail "preset conus: zooms $minz-$maxz, want 6 and up"
inside "$bbox" 38.9 -77.0 && inside "$bbox" 30 -120 || fail "preset conus: bbox $bbox does not cover the US"
[ "$(sed -n 3p "$tmp/calls")" = "$overlays_line --dry-run" ] || fail "preset conus: overlays call"
grep -q -- --container "$tmp/calls" && fail "preset conus: --container with GDAL on PATH"
[ -e "$maps" ] && fail "preset conus: --dry-run created the map dir"

# 1b. The base already in meta.json: only the preset is baked. --list
# prints the regions and runs no helper.
if [ $have_py = 1 ]; then
  mkdir -p "$maps/tiles"
  printf '%s\n' '{"minZoom":0,"maxZoom":9,"bbox":[-180,-85,180,85],"regions":[' \
    '{"name":"global","bbox":[-180,-85,180,85],"minZoom":0,"maxZoom":5},' \
    '{"name":"around","bbox":[-79,37,-75,41],"minZoom":6,"maxZoom":9}]}' > "$maps/tiles/meta.json"
  run "$GDAL" --config "$conf" --preset conus --dry-run
  [ $rc -eq 0 ] || fail "global present: exit $rc"
  [ "$(ncalls make-tiles.sh)" = 1 ] || fail "global present: want 1 make-tiles.sh call"
  call make-tiles.sh 1 | grep -q -- '--name conus ' || fail "global present: the call is not conus"
  [ "$(ncalls make-overlays.sh)" = 1 ] || fail "global present: want 1 make-overlays.sh call"
  run "$GDAL" --config "$conf" --list
  [ $rc -eq 0 ] || fail "--list: exit $rc"
  [ -s "$tmp/calls" ] && fail "--list called a helper"
  grep -q '^global ' "$tmp/out" && grep -q '^around ' "$tmp/out" || fail "--list: regions not printed"
  grep -q '0-5' "$tmp/out" && grep -q '6-9' "$tmp/out" || fail "--list: zoom ranges not printed"
  # --preset world is the base alone: nothing to bake once it is there.
  run "$GDAL" --config "$conf" --preset world --dry-run
  [ $rc -eq 0 ] && [ "$(ncalls make-tiles.sh)" = 0 ] && [ "$(ncalls make-overlays.sh)" = 1 ] ||
    fail "preset world with global present: exit $rc"
  # An old single-region meta.json is one region named default: the base is baked.
  printf '%s\n' '{"minZoom":0,"maxZoom":5,"bbox":[-180,-85,180,85]}' > "$maps/tiles/meta.json"
  run "$GDAL" --config "$conf" --preset conus --dry-run
  [ $rc -eq 0 ] && [ "$(sed -n 1p "$tmp/calls")" = "$global_line --dry-run" ] || fail "old meta.json: global not baked"
  run "$GDAL" --config "$conf" --list
  grep -q '^default ' "$tmp/out" || fail "--list: old meta.json not listed as default"
  printf '[1]\n' > "$maps/tiles/meta.json"
  run "$GDAL" --config "$conf" --preset conus --dry-run
  [ $rc -eq 1 ] && [ ! -s "$tmp/calls" ] || fail "malformed meta.json: exit $rc, want 1 and no helper run"
  rm -rf "$maps"
else
  echo "SKIP: python3 not installed (meta.json cases)"
fi
run "$GDAL" --config "$conf" --list
[ $rc -eq 0 ] && [ ! -s "$tmp/calls" ] || fail "--list without tiles: exit $rc"
grep -q 'no tiles' "$tmp/out" "$tmp/err" || fail "--list without tiles: says nothing"
# --preset world on a fresh dir: the base and the overlays only.
run "$GDAL" --config "$conf" --preset world --dry-run
[ $rc -eq 0 ] && [ "$(ncalls make-tiles.sh)" = 1 ] && [ "$(call make-tiles.sh 1)" = "$global_line --dry-run" ] ||
  fail "preset world: exit $rc"

# 2. --around RADIUS_NM: the receiver position from the config.
run "$GDAL" --config "$conf" --around 100 --dry-run
[ $rc -eq 0 ] || fail "--around 100: exit $rc"
[ "$(call make-tiles.sh 1)" = "$global_line --dry-run" ] || fail "--around 100: first call is not the global base"
read -r _ bbox minz maxz rest <<<"$(call make-tiles.sh 2)"
inside "$bbox" 38.9 -77.0 || fail "--around 100: bbox $bbox does not contain 38.9,-77.0"
inside "$bbox" 40.5 -77.0 && inside "$bbox" 37.3 -77.0 || fail "--around 100: bbox $bbox shorter than 100 nm"
inside "$bbox" 39.0 -79 && inside "$bbox" 39.0 -75 || fail "--around 100: bbox $bbox narrower than 100 nm"
inside "$bbox" 41.5 -77.0 && fail "--around 100: bbox $bbox much larger than 100 nm"
[ "$minz $maxz" = "6 9" ] || fail "--around 100: zooms $minz $maxz, want 6 9"
[ "$rest" = "--name around --out $maps/tiles --cache $cache --dry-run" ] || fail "--around 100: '$rest'"

# 3. --around LAT LON RADIUS_NM, a negative longitude among them.
run "$GDAL" --config "$conf" --around 51.5 -0.12 50 --dry-run
[ $rc -eq 0 ] || fail "--around 51.5 -0.12 50: exit $rc"
read -r _ bbox minz maxz rest <<<"$(call make-tiles.sh 2)"
inside "$bbox" 51.5 -0.12 || fail "--around 51.5 -0.12 50: bbox $bbox does not contain it"
inside "$bbox" 38.9 -77.0 && fail "--around 51.5 -0.12 50: bbox $bbox is around the configured receiver"
# Near a pole and the antimeridian the box is clamped, not wrapped.
run "$GDAL" --config "$conf" --around 84 179.5 300 --dry-run
[ $rc -eq 0 ] || fail "--around 84 179.5 300: exit $rc"
read -r _ bbox _ <<<"$(call make-tiles.sh 2)"
awk -v b="$bbox" 'BEGIN { split(b, a, ","); exit !(a[1] >= -180 && a[3] <= 180 && a[2] >= -85 && a[4] <= 85 && a[1] < a[3] && a[2] < a[4]) }' ||
  fail "--around 84 179.5 300: bbox $bbox not clamped"

# 4. MAP_DIR: --map-dir, else the config's last assignment (quotes removed),
# else exit 2 naming it. The receiver position likewise.
printf '%s\n' 'RECEIVER_LAT=38.9' 'RECEIVER_LON=-77.0' > "$tmp/nomap.conf"
run "$GDAL" --config "$tmp/nomap.conf" --preset conus --dry-run
[ $rc -eq 2 ] || fail "no MAP_DIR: exit $rc, want 2"
grep -q 'MAP_DIR' "$tmp/err" && grep -q -- '--map-dir' "$tmp/err" || fail "no MAP_DIR: stderr does not name MAP_DIR and --map-dir"
[ -s "$tmp/calls" ] && fail "no MAP_DIR: called a helper"
run "$GDAL" --config "$tmp/nomap.conf" --map-dir "$tmp/m2" --preset conus --dry-run
[ $rc -eq 0 ] && grep -q -- "--out $tmp/m2/tiles " "$tmp/calls" || fail "--map-dir: not used"
printf '%s\n' 'MAP_DIR=/nonexistent/one' "MAP_DIR=\"$tmp/m3\"" > "$tmp/twice.conf"
run "$GDAL" --config "$tmp/twice.conf" --preset conus --dry-run
[ $rc -eq 0 ] && grep -q -- "--out $tmp/m3/overlays " "$tmp/calls" || fail "MAP_DIR: the last, unquoted assignment is not used"
printf '%s\n' "MAP_DIR=$maps" 'RECEIVER_LAT=' > "$tmp/nopos.conf"
run "$GDAL" --config "$tmp/nopos.conf" --around 100 --dry-run
[ $rc -eq 2 ] && grep -q RECEIVER_LAT "$tmp/err" && grep -q RECEIVER_LON "$tmp/err" ||
  fail "--around without a position: exit $rc, want 2 naming RECEIVER_LAT and RECEIVER_LON"
run "$GDAL" --config "$tmp/nopos.conf" --around 10 20 30 --dry-run
[ $rc -eq 0 ] || fail "--around LAT LON R with no position in the config: exit $rc"
printf '%s\n' "MAP_DIR=\$HOME/maps" > "$tmp/bad.conf"
run "$GDAL" --config "$tmp/bad.conf" --preset conus --dry-run
[ $rc -eq 2 ] && grep -q "$tmp/bad.conf:1" "$tmp/err" || fail "malformed config: exit $rc, want 2 naming the line"
run "$GDAL" --config "$tmp/missing.conf" --preset conus --dry-run
[ $rc -eq 2 ] && grep -q "$tmp/missing.conf" "$tmp/err" || fail "missing config: exit $rc, want 2"

# 5. No GDAL and no container engine, a real run: exit 3 naming both routes.
run "$BARE" --config "$conf" --preset conus
[ $rc -eq 3 ] || fail "no GDAL: exit $rc, want 3"
grep -q 'gdal-bin python3-gdal' "$tmp/err" && grep -q -- '--container' "$tmp/err" ||
  fail "no GDAL: stderr does not name gdal-bin python3-gdal and --container"
grep -q 'EPEL' "$tmp/err" || fail "no GDAL: no RHEL-family hint"
[ -s "$tmp/calls" ] && fail "no GDAL: called a helper"

# 6. No GDAL, --dry-run: the container route's plan with the default image.
run "$BARE" --config "$conf" --preset conus --dry-run
[ $rc -eq 0 ] || fail "no GDAL dry-run: exit $rc"
[ "$(grep -c . "$tmp/calls")" = 3 ] || fail "no GDAL dry-run: want 3 calls"
[ "$(grep -c -- "--container $image --dry-run\$" "$tmp/calls")" = 3 ] ||
  fail "no GDAL dry-run: not every call has --container $image"
[ "$(call make-tiles.sh 1)" = "$global_line --container $image --dry-run" ] || fail "no GDAL dry-run: global call"
# --container with no IMAGE takes the default; with one, that one; a real
# run needs podman or docker.
run "$GDAL" --config "$conf" --preset conus --container --dry-run
[ $rc -eq 0 ] && [ "$(grep -c -- "--container $image " "$tmp/calls")" = 3 ] || fail "--container: not the default image"
run "$GDAL" --config "$conf" --preset conus --container img:2 --dry-run
[ $rc -eq 0 ] && [ "$(grep -c -- '--container img:2 ' "$tmp/calls")" = 3 ] || fail "--container img:2: not passed on"
run "$BARE" --config "$conf" --preset conus --container
[ $rc -eq 3 ] && grep -q 'podman or docker' "$tmp/err" && [ ! -s "$tmp/calls" ] ||
  fail "--container without an engine: exit $rc, want 3"
run "$tmp/engine:$BARE" --config "$conf" --preset conus --container img:2
[ $rc -eq 0 ] || fail "--container with podman, real run: exit $rc"
[ "$(sed -n 1p "$tmp/calls")" = "$global_line --container img:2" ] || fail "--container real run: global call"
grep -q -- '--dry-run' "$tmp/calls" && fail "real run: helpers got --dry-run"
[ -d "$maps" ] || fail "real run: map dir not created"
rm -rf "$maps"

# 7. An unwritable map dir: exit 1 before anything else, GDAL or not.
if [ "$(id -u)" = 0 ]; then
  echo "SKIP: running as root (unwritable map dir)"
else
  mkdir -p "$tmp/ro"
  printf '%s\n' "MAP_DIR=$tmp/ro/maps" 'RECEIVER_LAT=38.9' 'RECEIVER_LON=-77.0' > "$tmp/ro.conf"
  chmod 0555 "$tmp/ro"
  for p in "$GDAL" "$BARE"; do
    for dry in "" --dry-run; do
      run "$p" --config "$tmp/ro.conf" --preset conus $dry
      [ $rc -eq 1 ] || fail "unwritable map dir (PATH $p $dry): exit $rc, want 1"
      grep -q "cannot write $tmp/ro" "$tmp/err" || fail "unwritable map dir: stderr lacks 'cannot write'"
      [ -s "$tmp/calls" ] && fail "unwritable map dir: called a helper"
    done
  done
  mkdir -p "$tmp/ro2/maps" && chmod 0555 "$tmp/ro2/maps"
  run "$GDAL" --map-dir "$tmp/ro2/maps" --config "$conf" --preset conus --dry-run
  [ $rc -eq 1 ] && grep -q "cannot write $tmp/ro2/maps" "$tmp/err" || fail "unwritable existing map dir: exit $rc"
fi

# 7b. Not a map directory: / (also through a symlink), or a non-empty dir
# with neither tiles/ nor overlays/, is refused (exit 2) before any helper
# or chown, dry run or not, with --owner or without. An empty dir, or one
# that already has tiles/ or overlays/ beside other files, is accepted.
mkdir -p "$tmp/other" && echo keep > "$tmp/other/notes.txt"
ln -s / "$tmp/rootlink"
for d in / "$tmp/rootlink" "$tmp/rootlink/" "$tmp/other"; do
  for extra in --dry-run "--owner $(id -un)" "--owner $(id -un) --dry-run"; do
    # shellcheck disable=SC2086
    run "$GDAL" --config "$conf" --map-dir "$d" --preset conus $extra
    [ $rc -eq 2 ] || fail "map dir $d ($extra): exit $rc, want 2"
    want="refusing $d: it is not empty"
    [ "$d" = "$tmp/other" ] || want="refusing --map-dir /"
    grep -qF "$want" "$tmp/err" || fail "map dir $d: stderr lacks '$want'"
    [ -s "$tmp/calls" ] && fail "map dir $d: called a helper"
    grep -q 'chown' "$tmp/out" && fail "map dir $d: a chown in the plan"
  done
done
[ "$(cat "$tmp/other/notes.txt")" = keep ] || fail "refused map dir was changed"
mkdir -p "$tmp/empty" "$tmp/old/tiles" "$tmp/oldo/overlays"
echo x > "$tmp/old/README" && echo x > "$tmp/oldo/README"
for d in "$tmp/empty" "$tmp/old" "$tmp/oldo"; do
  run "$GDAL" --config "$conf" --map-dir "$d" --preset conus --dry-run
  [ $rc -eq 0 ] || fail "map dir $d: exit $rc, want 0"
done
rm -f "$tmp/rootlink"
# 7d. A symlink where the bake writes is refused (exit 2) before any helper or
# chown, dry run or not, with --owner or without: the map dir itself (also
# given with a trailing slash), its tiles/ or overlays/ (also dangling), and
# tiles/meta.json. The symlinks' targets are left alone.
mkdir -p "$tmp/target/tiles" "$tmp/lm" "$tmp/lt/maps" "$tmp/lo/maps" "$tmp/lj/maps/tiles" "$tmp/ld/maps"
echo keep > "$tmp/target/keep"
echo '{"regions":[]}' > "$tmp/target/meta.json"
ln -s "$tmp/target" "$tmp/lm/maps"
ln -s "$tmp/target" "$tmp/lt/maps/tiles"
ln -s "$tmp/target" "$tmp/lo/maps/overlays"
ln -s "$tmp/target/meta.json" "$tmp/lj/maps/tiles/meta.json"
ln -s "$tmp/nowhere" "$tmp/ld/maps/tiles"
for c in "lm/maps:lm/maps" "lm/maps/:lm/maps" "lt/maps:lt/maps/tiles" "lo/maps:lo/maps/overlays" \
         "lj/maps:lj/maps/tiles/meta.json" "ld/maps:ld/maps/tiles"; do
  d=$tmp/${c%%:*} link=$tmp/${c#*:}
  for extra in "" --dry-run "--owner $(id -un)" "--owner $(id -un) --dry-run"; do
    # shellcheck disable=SC2086
    run "$GDAL" --config "$conf" --map-dir "$d" --preset conus $extra
    [ $rc -eq 2 ] || fail "symlink $link ($extra): exit $rc, want 2"
    grep -qF "refusing $link: it is a symlink" "$tmp/err" || fail "symlink $link: stderr lacks 'refusing $link: it is a symlink'"
    [ -s "$tmp/calls" ] && fail "symlink $link: called a helper"
    grep -q 'chown' "$tmp/out" && fail "symlink $link: a chown in the plan"
  done
done
[ "$(ls "$tmp/target" | tr '\n' ' ')" = "keep meta.json tiles " ] || fail "a symlink's target was changed: $(ls "$tmp/target")"
[ "$(cat "$tmp/target/meta.json")" = '{"regions":[]}' ] || fail "meta.json behind a symlink was changed"
[ -e "$tmp/nowhere" ] && fail "a dangling symlink's target was created"
rm -rf "$tmp/target" "$tmp/lm" "$tmp/lt" "$tmp/lo" "$tmp/lj" "$tmp/ld"
# 7c. A map dir that cannot be entered (no search permission) stops with exit 2
# and says so, before any helper runs. (root enters any directory.)
if [ "$(id -u)" != 0 ]; then
  mkdir -p "$tmp/noenter" && chmod 0600 "$tmp/noenter"
  run "$GDAL" --config "$conf" --map-dir "$tmp/noenter" --preset conus --dry-run
  chmod 0755 "$tmp/noenter"
  [ $rc -eq 2 ] || fail "unenterable map dir: exit $rc, want 2"
  grep -qF "cannot enter the map dir $tmp/noenter" "$tmp/err" || fail "unenterable map dir: stderr lacks 'cannot enter'"
  [ -s "$tmp/calls" ] && fail "unenterable map dir: called a helper"
fi

# 8. Usage errors exit 2 and run nothing.
for args in "--preset nowhere" "" "--preset" "--preset conus --around 100" "--around" "--around 1 2" \
            "--around x" "--around 0" "--around 91 0 10" "--around 0 181 10" "--around 100000" \
            "--bbox -10,35,30,60" "--zoom 6-8" "--bbox -10,35,30 --zoom 6-8" "--bbox -10,35,30,60 --zoom 8-6" \
            "--bbox -10,35,30,60 --zoom 6" "--bbox -10,35,30,60 --zoom 6-19" "--preset conus --bogus" \
            "--preset conus --owner nosuchuser-skycue"; do
  # shellcheck disable=SC2086
  run "$GDAL" --config "$conf" $args --dry-run
  [ $rc -eq 2 ] || fail "usage '$args': exit $rc, want 2"
  [ -s "$tmp/calls" ] && fail "usage '$args': called a helper"
done

# 9. --bbox W,S,E,N --zoom MINZ-MAXZ, an explicit --cache and --owner.
run "$GDAL" --config "$conf" --bbox -10,35,30,60 --zoom 6-8 --cache "$tmp/c" --owner "$(id -un)" --dry-run
[ $rc -eq 0 ] || fail "--bbox: exit $rc"
[ "$(call make-tiles.sh 2)" = "make-tiles.sh -10,35,30,60 6 8 --name bbox --out $maps/tiles --cache $tmp/c --dry-run" ] ||
  fail "--bbox: second call"
grep -qxF "would run: chown -R $(id -un): $maps" "$tmp/out" || fail "--owner: no chown in the plan"

# 10. A failing helper stops the run with its status.
export STUB_RC=1
run "$GDAL" --config "$conf" --preset conus --dry-run
unset STUB_RC
[ $rc -eq 1 ] && [ "$(grep -c . "$tmp/calls")" = 1 ] || fail "failing helper: exit $rc, want 1 after one call"

echo "PASS test_make_maps"
