#!/usr/bin/env bash
# make-tiles.sh BBOX MINZ MAXZ [--name NAME] [--out DIR] [--cache DIR] [--container IMAGE] [--palette] [--meta-only] [--dry-run]
# Builds offline map tiles for the skycue web UI from the Natural Earth
# 1:10m raster (NE1_HR_LC_SR_W_DR, public domain). Run it on a connected
# machine: it downloads the raster (about 310 MB) once into the cache.
#   BBOX  west,south,east,north in degrees, e.g. -10,35,30,60
#   MINZ, MAXZ  zoom range, 0..18 (see scripts/make-tiles.md for sizes)
#   --name NAME  the region's name in meta.json (default: default)
#   --out DIR    tile directory (default: skycue/www/tiles in this repo)
#   --cache DIR  where the raster is kept (default: build/naturalearth)
#   --container IMAGE  run gdal_translate and gdal2tiles.py in this GDAL
#                image with podman (or docker), e.g. the one
#                skycue-make-maps uses, ghcr.io/osgeo/gdal:ubuntu-small-3.9.3,
#                instead of the ones on PATH; as make-overlays.sh, it writes
#                as your user with the cache bind-mounted (:Z). The download
#                and the merge stay on this machine.
#   --palette    write 256-colour PNGs (about 1/7 the size; needs Pillow).
#                The merge runs tiles-merge.py under `python3 -I`, which
#                ignores user site-packages: Pillow must be importable by
#                the isolated interpreter (`python3 -I -c 'import PIL'`) on
#                the machine that bakes the tiles, not only with --user.
#   --meta-only  only add the region to meta.json (tiles already there)
#   --dry-run    print the commands and the region, run nothing
# Runs add up: the new tiles are merged into DIR (tiles already there are
# kept; where two regions meet, their edge tiles are composited) and
# meta.json lists every region, with the overall zoom range at the top.
# Needs gdal2tiles.py and gdal_translate (GDAL, with its Python bindings; or
# --container), python3, plus curl and unzip for the download. Exit status:
# 0 done, 2 usage, 3 a required tool is missing, 1 anything else.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
common="$(dirname "$0")/lib/skycue-common.sh"
[ -f "$common" ] || { echo "make-tiles: $common not found" >&2; exit 1; }
# shellcheck source=lib/skycue-common.sh
. "$common" # SKYCUE_GDAL_IMAGE
NE_NAME=NE1_HR_LC_SR_W_DR
NE_URL=${SKYCUE_NE_URL:-https://naturalearth.s3.amazonaws.com/10m_raster/${NE_NAME}.zip}

die() { echo "make-tiles: $*" >&2; exit 1; }
usage() {
  echo "usage: $(basename "$0") WEST,SOUTH,EAST,NORTH MINZ MAXZ [--name NAME] [--out DIR] [--cache DIR] [--container IMAGE] [--palette] [--meta-only] [--dry-run]" >&2
  exit 2
}
bad() { echo "make-tiles: $*" >&2; usage; }
gdal_hint() {
  cat >&2 <<EOF
make-tiles: $1 not found on PATH. It comes with GDAL's Python tools:
  dnf (with EPEL):     dnf install gdal python3-gdal
  apt:                 apt install gdal-bin python3-gdal
  conda:               conda install -c conda-forge gdal
(gdal2tiles.py --xyz needs GDAL 3.1 or newer.)
or run them from a GDAL container image: --container $SKYCUE_GDAL_IMAGE
EOF
  exit 3
}

out="$repo/skycue/www/tiles"
cache="$repo/build/naturalearth"
dry=0
metaonly=0
palette=()
name=default
pos=()
image=
while [ $# -gt 0 ]; do
  case $1 in
    --out) [ $# -ge 2 ] || usage; out=$2; shift ;;
    --cache) [ $# -ge 2 ] || usage; cache=$2; shift ;;
    --container) [ $# -ge 2 ] && [ -n "$2" ] && [[ $2 != -* ]] || bad "--container needs an IMAGE"; image=$2; shift ;;
    --name) [ $# -ge 2 ] || usage; name=$2; shift ;;
    --meta-only) metaonly=1 ;;
    --palette) palette=(--palette) ;;
    --dry-run) dry=1 ;;
    -h|--help) usage ;;
    -[0-9.]*) pos+=("$1") ;; # a BBOX starting with a negative west
    -*) bad "unknown option $1" ;;
    *) pos+=("$1") ;;
  esac
  shift
done
[ ${#pos[@]} -eq 3 ] || usage
bbox=${pos[0]} minz=${pos[1]} maxz=${pos[2]}

num='-?([0-9]+(\.[0-9]*)?|\.[0-9]+)'
[[ $bbox =~ ^($num),($num),($num),($num)$ ]] || bad "BBOX must be west,south,east,north in degrees, got '$bbox'"
IFS=, read -r west south east north <<<"$bbox"
awk -v w="$west" -v s="$south" -v e="$east" -v n="$north" \
  'BEGIN { exit !(w >= -180 && e <= 180 && w < e && s >= -90 && n <= 90 && s < n) }' ||
  bad "BBOX needs -180 <= west < east <= 180 and -90 <= south < north <= 90"
for z in "$minz" "$maxz"; do
  [[ $z =~ ^[0-9]+$ ]] && [ "$z" -le 18 ] || bad "zoom levels must be whole numbers 0..18, got '$z'"
done
[ "$minz" -le "$maxz" ] || bad "MINZ ($minz) is above MAXZ ($maxz)"
[[ $name =~ ^[A-Za-z0-9_-]{1,40}$ ]] || bad "NAME must be 1..40 letters, digits, _ or -, got '$name'"

engine=
if [ $metaonly = 1 ]; then
  :
elif [ -n "$image" ]; then
  # The engine choice and user mapping of make-overlays.sh: the container
  # writes as the calling user, so the next run can remove what it left
  # (rootless podman with --userns=keep-id, docker and rootful podman with
  # --user). With --dry-run no engine is needed.
  for e in podman docker; do command -v $e >/dev/null 2>&1 && { engine=$e; break; }; done
  [ -n "$engine" ] || [ $dry = 1 ] || { echo "make-tiles: --container needs podman or docker on PATH" >&2; exit 3; }
  engine=${engine:-podman}
  # Absolute, so -v gets a bind mount, not a named volume.
  case $cache in /*) ;; *) cache="$PWD/$cache" ;; esac
else
  command -v gdal2tiles.py >/dev/null 2>&1 || gdal_hint gdal2tiles.py
  command -v gdal_translate >/dev/null 2>&1 || gdal_hint gdal_translate
fi
userargs=()
if [ "$engine" = podman ] && [ "$(id -u)" != 0 ]; then userargs=(--userns=keep-id)
elif [ -n "$engine" ]; then userargs=(--user "$(id -u):$(id -g)"); fi

zip="$cache/$NE_NAME.zip"
tif="$cache/$NE_NAME.tif"
crop="$cache/crop-${west}_${south}_${east}_${north}.tif"
stage="$cache/stage-$name"
merger="$repo/scripts/tiles-merge.py"
generated=$(date -u +%Y-%m-%dT%H:%M:%SZ)
region=$(printf '{"name":"%s","bbox":[%s,%s,%s,%s],"minZoom":%d,"maxZoom":%d}' \
  "$name" "$west" "$south" "$east" "$north" "$minz" "$maxz")

# step CMD...: print it (--dry-run) or run it, stopping on failure.
step() {
  if [ $dry = 1 ]; then echo "would run: $*"; return; fi
  echo "make-tiles: $*"
  "$@" || die "failed: $*"
}
need() { # need TOOL WHY: checked only when used, and not for --dry-run
  [ $dry = 1 ] || command -v "$1" >/dev/null 2>&1 || { echo "make-tiles: $1 not found on PATH ($2)" >&2; exit 3; }
}
need python3 "needed to merge the tiles and write meta.json"
# write_meta: add this region to meta.json
write_meta() {
  if [ $dry = 1 ]; then
    echo "would add to $out/meta.json: $region"
    return
  fi
  python3 -I "$merger" meta "$out/meta.json" "$name" "$west" "$south" "$east" "$north" \
    "$minz" "$maxz" "$generated" || die "failed to write $out/meta.json"
}

if [ $metaonly = 1 ]; then
  [ $dry = 1 ] || mkdir -p "$out"
  write_meta
  exit 0
fi

# A broken meta.json stops the run before any tile is cut.
if [ $dry = 0 ]; then
  python3 -I "$merger" check "$out/meta.json" || exit 1
fi
[ $dry = 1 ] || mkdir -p "$cache" "$out"
if [ ! -f "$tif" ]; then
  if [ ! -f "$zip" ]; then
    need curl "needed to download the Natural Earth raster"
    step curl -fL --retry 3 -o "$zip.part" "$NE_URL"
    step mv "$zip.part" "$zip"
  fi
  need unzip "needed to unpack the Natural Earth raster"
  step unzip -o -j "$zip" "$NE_NAME.tif" -d "$cache"
fi
# The raster is in EPSG:4326, so -projwin takes the bbox as is (upper left,
# lower right); gdal2tiles reprojects to Web Mercator itself. The tiles are
# cut into a staging directory and then merged, so earlier runs survive.
# With --container, both run in the image with the cache mounted at /c as
# the working directory, so they get paths relative to the cache.
[ $dry = 1 ] || rm -rf "$stage"
gdal() { # gdal TOOL ARGS...
  if [ -n "$engine" ]; then
    step "$engine" run --rm --network=none ${userargs[@]+"${userargs[@]}"} -v "$cache:/c:Z" -w /c "$image" "$@"
  else
    step "$@"
  fi
}
if [ -n "$engine" ]; then in_tif=${tif##*/} in_crop=${crop##*/} in_stage=${stage##*/}
else in_tif=$tif in_crop=$crop in_stage=$stage; fi
gdal gdal_translate -q -projwin "$west" "$north" "$east" "$south" "$in_tif" "$in_crop"
gdal gdal2tiles.py --xyz -z "$minz-$maxz" -w none "$in_crop" "$in_stage"
step python3 -I "$merger" merge "$stage" "$out" ${palette[@]+"${palette[@]}"}
write_meta
[ $dry = 1 ] && exit 0
rm -rf "$crop" "$stage"
echo "make-tiles: wrote $out ($(du -sh "$out" | cut -f1)), region $name zoom $minz-$maxz"
