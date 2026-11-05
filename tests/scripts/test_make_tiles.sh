#!/usr/bin/env bash
# make-tiles.sh without GDAL: argument checks, exit 3 with the install hint
# when gdal2tiles.py is missing, and --dry-run command lines with fake GDAL
# tools on PATH. Runs nothing that needs the network.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
script="$here/scripts/make-tiles.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: test_make_tiles: $*"; [ -f "$tmp/out" ] && cat "$tmp/out" "$tmp/err"; exit 1; }
[ -f "$script" ] || fail "scripts/make-tiles.sh missing"
bash -n "$script" || fail "bash -n"
bash_bin=$(command -v bash)

# A PATH holding only the basic tools the script uses (so a GDAL installed
# on this machine cannot leak in), plus optionally fake GDAL tools.
mkdir -p "$tmp/tools" "$tmp/fake"
for t in awk basename cat cut date dirname du id mkdir mv rm; do
  p=$(command -v "$t") || fail "no $t on this machine"
  ln -s "$p" "$tmp/tools/$t"
done
for t in gdal2tiles.py gdal_translate; do
  printf '#!/bin/sh\necho "fake %s $*" >&2\nexit 1\n' "$t" > "$tmp/fake/$t"
  chmod +x "$tmp/fake/$t"
done

run() { # run PATH ARGS...: exit status in $rc, output in $tmp/out, $tmp/err
  local p=$1; shift
  PATH=$p "$bash_bin" "$script" "$@" >"$tmp/out" 2>"$tmp/err"; rc=$?
}

# 1. No GDAL: exit 3 and the hint, for a real run and for --dry-run.
for dry in "" --dry-run; do
  run "$tmp/tools" -10,35,30,60 0 6 --out "$tmp/tiles" --cache "$tmp/cache" $dry
  [ $rc -eq 3 ] || fail "no GDAL ${dry}: exit $rc, want 3"
  grep -q 'gdal2tiles.py not found' "$tmp/err" || fail "no GDAL: does not name gdal2tiles.py"
  grep -q 'python3-gdal' "$tmp/err" || fail "no GDAL: no install hint naming the GDAL Python tools package"
  [ -e "$tmp/tiles" ] && fail "no GDAL: created the output directory"
done

# 2. Usage errors exit 2 (checked before the tools).
for args in "" "-10,35,30 0 6" "30,35,-10,60 0 6" "-10,35,30,60 7 6" "-10,35,30,60 0 19" \
            "-10,35,30,60 0 x" "-10,35,30,60 0 6 --bogus"; do
  # shellcheck disable=SC2086
  run "$tmp/fake:$tmp/tools" $args
  [ $rc -eq 2 ] || fail "usage '$args': exit $rc, want 2"
done

# 3. --dry-run with fake GDAL: the command lines, nothing created.
run "$tmp/fake:$tmp/tools" -10.5,35,30,60.25 0 6 --out "$tmp/tiles" --cache "$tmp/cache" --dry-run
[ $rc -eq 0 ] || fail "dry-run: exit $rc"
ne=NE1_HR_LC_SR_W_DR
expect=(
  "would run: curl -fL --retry 3 -o $tmp/cache/$ne.zip.part https://naturalearth.s3.amazonaws.com/10m_raster/$ne.zip"
  "would run: mv $tmp/cache/$ne.zip.part $tmp/cache/$ne.zip"
  "would run: unzip -o -j $tmp/cache/$ne.zip $ne.tif -d $tmp/cache"
  "would run: gdal_translate -q -projwin -10.5 60.25 30 35 $tmp/cache/$ne.tif $tmp/cache/crop--10.5_35_30_60.25.tif"
  "would run: gdal2tiles.py --xyz -z 0-6 -w none $tmp/cache/crop--10.5_35_30_60.25.tif $tmp/cache/stage-default"
  "would run: python3 -I $here/scripts/tiles-merge.py merge $tmp/cache/stage-default $tmp/tiles"
)
i=0
while IFS= read -r line; do
  case $line in
    "would add to "*) break ;;
  esac
  [ "$line" = "${expect[$i]:-}" ] || fail "dry-run line $((i + 1)): got '$line', want '${expect[$i]:-<none>}'"
  i=$((i + 1))
done < "$tmp/out"
[ $i -eq ${#expect[@]} ] || fail "dry-run: $i command lines, want ${#expect[@]}"
grep -qx "would add to $tmp/tiles/meta.json: {\"name\":\"default\",\"bbox\":\[-10.5,35,30,60.25\],\"minZoom\":0,\"maxZoom\":6}" "$tmp/out" ||
  fail "dry-run: no meta.json region line with name/bbox/minZoom/maxZoom"
[ -e "$tmp/tiles" ] || [ -e "$tmp/cache" ] && fail "dry-run created files"
run "$tmp/fake:$tmp/tools" -10,35,30,60 0 6 --name conus --cache "$tmp/cache" --dry-run
grep -q "stage-conus" "$tmp/out" && grep -q '"name":"conus"' "$tmp/out" || fail "dry-run --name: not in the staging dir or region"
for nm in "" "a/b" "x y"; do
  run "$tmp/fake:$tmp/tools" -10,35,30,60 0 6 --name "$nm" --dry-run
  [ $rc -eq 2 ] || fail "--name '$nm': exit $rc, want 2"
done

# 4. With the raster already cached, no download or unzip step.
mkdir -p "$tmp/cache" && : > "$tmp/cache/$ne.tif"
run "$tmp/fake:$tmp/tools" -10,35,30,60 2 4 --out "$tmp/tiles" --cache "$tmp/cache" --dry-run
[ $rc -eq 0 ] || fail "dry-run cached: exit $rc"
grep -Eq '^would run: (curl|mv|unzip) ' "$tmp/out" && fail "dry-run cached: still downloads"
grep -q "^would run: gdal2tiles.py --xyz -z 2-4 -w none " "$tmp/out" || fail "dry-run cached: no gdal2tiles line"

# 4b. --container IMAGE with only the basic tools on PATH (no GDAL, no
# podman or docker): --dry-run needs no engine and prints the GDAL steps as
# podman runs in IMAGE, with the cache mounted at /c and paths relative to
# it; the merge stays on the host. A real run without an engine exits 3.
rm -rf "$tmp/cache"
run "$tmp/tools" -10,35,30,60 0 6 --container img:1 --out "$tmp/tiles" --cache "$tmp/cache" --dry-run
[ $rc -eq 0 ] || fail "--container dry-run: exit $rc, want 0"
if [ "$(id -u)" = 0 ]; then ua="--user $(id -u):$(id -g)"; else ua=--userns=keep-id; fi
crun="would run: podman run --rm --network=none $ua -v $tmp/cache:/c:Z -w /c img:1"
grep -qxF "$crun gdal_translate -q -projwin -10 60 30 35 $ne.tif crop--10_35_30_60.tif" "$tmp/out" ||
  fail "--container dry-run: no '$crun gdal_translate ...' line"
grep -qxF "$crun gdal2tiles.py --xyz -z 0-6 -w none crop--10_35_30_60.tif stage-default" "$tmp/out" ||
  fail "--container dry-run: no '$crun gdal2tiles.py ...' line"
grep -qxF "would run: python3 -I $here/scripts/tiles-merge.py merge $tmp/cache/stage-default $tmp/tiles" "$tmp/out" ||
  fail "--container dry-run: the merge is not on the host"
[ "$(grep -c '^would run: podman run --rm --network=none ' "$tmp/out")" = 2 ] ||
  fail "--container dry-run: want exactly two container steps"
[ -e "$tmp/tiles" ] || [ -e "$tmp/cache" ] && fail "--container dry-run created files"
# A relative cache is made absolute, so -v gets a bind mount.
(cd "$tmp" && PATH=$tmp/tools "$bash_bin" "$script" -10,35,30,60 0 6 --container img:1 --cache rel --dry-run) \
  >"$tmp/out" 2>"$tmp/err" || fail "--container relative cache: exit $?"
grep -qF -- "-v $tmp/rel:/c:Z " "$tmp/out" || fail "--container relative cache: not mounted by its absolute path"
run "$tmp/tools" -10,35,30,60 0 6 --container img:1 --out "$tmp/tiles" --cache "$tmp/cache"
[ $rc -eq 3 ] || fail "--container without podman or docker: exit $rc, want 3"
grep -q 'podman or docker' "$tmp/err" || fail "--container without an engine: does not name podman or docker"
[ -e "$tmp/tiles" ] && fail "--container without an engine: created the output directory"
run "$tmp/tools" -10,35,30,60 0 6 --container
[ $rc -eq 2 ] || fail "--container without IMAGE: exit $rc, want 2"
run "$tmp/tools" -10,35,30,60 0 6 --container ""
[ $rc -eq 2 ] || fail "--container '': exit $rc, want 2"
# The no-GDAL hint names the container route with the shared default image.
run "$tmp/tools" -10,35,30,60 0 6 --out "$tmp/tiles" --cache "$tmp/cache"
grep -qF -- '--container ghcr.io/osgeo/gdal:ubuntu-small-3.9.3' "$tmp/err" || fail "no GDAL: the hint does not name --container IMAGE"

# 5. meta.json regions with --meta-only (python3, no GDAL): an old
# single-region meta.json becomes "default", regions add up, the same name
# (or the same bbox and zooms) replaces an entry, the top level keeps the
# overall zoom range; no tile is touched.
if p=$(command -v python3); then
  ln -s "$p" "$tmp/tools/python3"
  m=$tmp/meta/meta.json
  mkdir -p "$tmp/meta/3/1" && : > "$tmp/meta/3/1/2.png"
  printf '%s\n' '{"minZoom":0,"maxZoom":9,"bbox":[-76,38.5,-66,46],"source":"Natural Earth 1:10m","generated":"x"}' > "$m"
  run "$tmp/tools" -76,38.5,-66,46 0 9 --name northeast --out "$tmp/meta" --meta-only
  [ $rc -eq 0 ] || fail "meta-only: exit $rc"
  run "$tmp/tools" -180,-85,180,85 0 5 --name global --out "$tmp/meta" --meta-only
  run "$tmp/tools" -161,18.5,-154.5,22.5 6 8 --name hawaii --out "$tmp/meta" --meta-only
  run "$tmp/tools" -161,18.5,-154.5,22.5 6 10 --name hawaii --out "$tmp/meta" --meta-only
  [ $rc -eq 0 ] || fail "meta-only: exit $rc"
  [ -e "$tmp/meta/3/1/2.png" ] || fail "meta-only: removed a tile"
  python3 -I - "$m" <<'EOP' || fail "meta-only: meta.json is $(cat "$m")"
import json, sys
m = json.load(open(sys.argv[1]))
assert (m["minZoom"], m["maxZoom"]) == (0, 10), m
assert m["bbox"] == [-180, -85, 180, 85], m
assert [(r["name"], r["minZoom"], r["maxZoom"]) for r in m["regions"]] == \
    [("northeast", 0, 9), ("global", 0, 5), ("hawaii", 6, 10)], m["regions"]
assert m["regions"][0]["bbox"] == [-76, 38.5, -66, 46]
assert m["source"] == "Natural Earth 1:10m" and m["generated"].endswith("Z")
EOP
  # merge: a tile the output lacks is moved in, the ones it has stay.
  mkdir -p "$tmp/stage/3/1" "$tmp/stage/4/2"
  echo new > "$tmp/stage/4/2/5.png"
  python3 -I "$here/scripts/tiles-merge.py" merge "$tmp/stage" "$tmp/meta" >/dev/null || fail "merge: exit $?"
  [ "$(cat "$tmp/meta/4/2/5.png")" = new ] && [ -e "$tmp/meta/3/1/2.png" ] || fail "merge: tiles not merged"

  # Malformed meta.json: a region entry without fields is dropped with a
  # warning; a top level that is not an object stops the run, before any
  # tile is cut (the fake gdal_translate would say "fake") and unchanged.
  printf '%s\n' '{"minZoom":0,"maxZoom":5,"regions":[{"name":"x"},{"name":"g","bbox":[-180,-85,180,85],"minZoom":0,"maxZoom":5}]}' > "$m"
  run "$tmp/tools" -10,35,30,60 2 3 --name eu --out "$tmp/meta" --meta-only
  [ $rc -eq 0 ] || fail "malformed region: exit $rc"
  grep -q 'dropping malformed region {"name": "x"}' "$tmp/err" || fail "malformed region: no warning"
  python3 -I -c 'import json,sys; m=json.load(open(sys.argv[1])); assert [r["name"] for r in m["regions"]] == ["g", "eu"], m' "$m" ||
    fail "malformed region: $(cat "$m")"
  printf '%s\n' '[1,2]' > "$m"
  mkdir -p "$tmp/cache2" && : > "$tmp/cache2/$ne.tif"
  run "$tmp/fake:$tmp/tools" -10,35,30,60 2 3 --out "$tmp/meta" --cache "$tmp/cache2"
  [ $rc -eq 1 ] || fail "non-object meta.json: exit $rc, want 1"
  grep -q 'is not a JSON object' "$tmp/err" || fail "non-object meta.json: no message"
  grep -q 'fake gdal' "$tmp/err" && fail "non-object meta.json: cut tiles first"
  [ "$(cat "$m")" = '[1,2]' ] || fail "non-object meta.json: rewritten"

  # Compositing and --palette (Pillow only): an existing tile keeps its
  # opaque half and gets the new tile in its transparent half. Every other
  # python3 here runs with -I; this block drops it when Pillow is only in the
  # user's site-packages (pip install --user), which -I leaves off sys.path,
  # so a dev machine still runs the check instead of skipping it.
  pil_flags=(-I)
  if ! python3 -I -c 'import PIL' 2>/dev/null && python3 -c 'import PIL' 2>/dev/null; then
    echo "NOTE: Pillow is only in user site-packages; running the compositing check without -I"
    pil_flags=()
  fi
  if python3 ${pil_flags[@]+"${pil_flags[@]}"} -c 'import PIL' 2>/dev/null; then
    python3 ${pil_flags[@]+"${pil_flags[@]}"} - "$tmp/pil" "$here/scripts/tiles-merge.py" \
      ${pil_flags[@]+"${pil_flags[@]}"} <<'EOP' || fail "Pillow merge"
import os, subprocess, sys
from PIL import Image
d, merger, flags = sys.argv[1], sys.argv[2], sys.argv[3:]   # flags: the python flags this runs with
for sub in ("out/5/1", "stage/5/1", "stage/5/2"):
    os.makedirs(os.path.join(d, sub))
old = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
old.paste((200, 0, 0, 255), (0, 0, 128, 256))           # left half red
old.save(os.path.join(d, "out/5/1/1.png"))
Image.new("RGBA", (256, 256), (0, 0, 200, 255)).save(os.path.join(d, "stage/5/1/1.png"))  # blue
Image.new("RGBA", (256, 256), (0, 200, 0, 255)).save(os.path.join(d, "stage/5/2/1.png"))  # green, new
subprocess.check_call([sys.executable] + flags + [merger, "merge", os.path.join(d, "stage"), os.path.join(d, "out"),
                       "--palette"], stdout=subprocess.DEVNULL)
m = Image.open(os.path.join(d, "out/5/1/1.png"))
assert m.mode == "P", m.mode
m = m.convert("RGBA")
assert m.getpixel((10, 10)) == (200, 0, 0, 255) and m.getpixel((200, 10)) == (0, 0, 200, 255), \
    (m.getpixel((10, 10)), m.getpixel((200, 10)))
n = Image.open(os.path.join(d, "out/5/2/1.png"))
assert n.mode == "P" and n.convert("RGBA").getpixel((5, 5)) == (0, 200, 0, 255)
assert not os.path.exists(os.path.join(d, "stage/5/2/1.png"))
EOP
  else
    echo "SKIP: Pillow not installed (tile compositing, --palette)"
  fi
else
  echo "SKIP: python3 not installed (meta.json regions)"
fi

echo "PASS test_make_tiles"
