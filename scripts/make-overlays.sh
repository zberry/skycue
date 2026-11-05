#!/usr/bin/env bash
# make-overlays.sh [--cache DIR] [--out DIR] [--container IMAGE] [--dry-run]
# Builds the web UI's map overlays (Borders, Cities) from Natural Earth
# vectors (public domain): three small GeoJSON files and a meta.json.
#   admin0.json  ne_50m_admin_0_boundary_lines_land (country borders)
#   admin1.json  ne_50m_admin_1_states_provinces_lines, USA/CAN/MEX only
#   cities.json  ne_10m_populated_places_simple: scalerank <= 7 worldwide
#                and pop_max >= 50000 in USA/CAN/MEX; name, rank, pop
#   --cache DIR        where the downloads are kept (default: build/naturalearth)
#   --out DIR          output directory (default: www/overlays in this repo)
#   --container IMAGE  run ogr2ogr in this GDAL image with podman (or docker),
#                      e.g. ghcr.io/osgeo/gdal:ubuntu-small-3.9.3, instead of
#                      the one on PATH (the default when ogr2ogr is missing
#                      is to stop and say so); it writes as your user, with
#                      the cache bind-mounted (:Z, relabelled for SELinux)
#   --dry-run          print the commands, run nothing
# The downloads (about 1.2 MB in all) are pinned by SHA-256 and fetched only
# when the cache does not already hold a matching copy, so a second run, or a
# machine with a filled cache, needs no network. Set SKYCUE_NE_VECTOR_URL to
# a mirror holding the same zip files (the base URL, without the file name).
# Needs ogr2ogr (GDAL; or --container), python3, sha256sum, unzip, and curl
# for a download. Exit status: 0 done, 2 usage, 3 a required tool is missing,
# 1 anything else.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
common="$(dirname "$0")/lib/skycue-common.sh"
[ -f "$common" ] || { echo "make-overlays: $common not found" >&2; exit 1; }
# shellcheck source=lib/skycue-common.sh
. "$common" # SKYCUE_GDAL_IMAGE
NE_BASE=${SKYCUE_NE_VECTOR_URL:-https://naturalearth.s3.amazonaws.com}
# name  url-directory  version  sha256 of the zip
SOURCES=(
  "ne_50m_admin_0_boundary_lines_land 50m_cultural 5.1.0 02f369ae10578c4ce33fc4a08f8cdd69ea11eb39b44d7923e88eb7199aaff8ea"
  "ne_50m_admin_1_states_provinces_lines 50m_cultural 5.1.0 464e762020d198257a07f5f62eb7cbfc287fccbc0a6db788d8068b49844d951e"
  "ne_10m_populated_places_simple 10m_cultural 5.1.2 2963ee08f7743f13f37d5243acd77a2f7a510e26a36c7e04b920f1b381f2b251"
)
SIMPLIFY_DEG=0.01   # line simplification tolerance (about 1 km)

die() { echo "make-overlays: $*" >&2; exit 1; }
usage() {
  echo "usage: $(basename "$0") [--cache DIR] [--out DIR] [--container IMAGE] [--dry-run]" >&2
  exit 2
}
bad() { echo "make-overlays: $*" >&2; usage; }

out="$repo/www/overlays"
cache="$repo/build/naturalearth"
image=
dry=0
while [ $# -gt 0 ]; do
  case $1 in
    --out) [ $# -ge 2 ] && [ -n "$2" ] || usage; out=$2; shift ;;
    --cache) [ $# -ge 2 ] && [ -n "$2" ] || usage; cache=$2; shift ;;
    --container) [ $# -ge 2 ] && [ -n "$2" ] || usage; image=$2; shift ;;
    --dry-run) dry=1 ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) bad "unknown argument $1" ;;
  esac
  shift
done
# Absolute, so the container's -v gets a bind mount, not a named volume.
abspath() { case $1 in /*) printf '%s\n' "$1" ;; *) printf '%s\n' "$PWD/$1" ;; esac; }
cache=$(abspath "$cache")
out=$(abspath "$out")

need() { # need TOOL WHY: not checked for --dry-run
  [ $dry = 1 ] || command -v "$1" >/dev/null 2>&1 || { echo "make-overlays: $1 not found on PATH ($2)" >&2; exit 3; }
}
engine=
if [ -n "$image" ]; then
  for e in podman docker; do command -v $e >/dev/null 2>&1 && { engine=$e; break; }; done
  [ -n "$engine" ] || [ $dry = 1 ] || { echo "make-overlays: --container needs podman or docker on PATH" >&2; exit 3; }
  engine=${engine:-podman}
elif [ $dry = 0 ] && ! command -v ogr2ogr >/dev/null 2>&1; then
  cat >&2 <<EOF
make-overlays: ogr2ogr not found on PATH. It comes with GDAL:
  dnf (with EPEL):  dnf install gdal
  apt:              apt install gdal-bin
or run it from a GDAL container image: --container $SKYCUE_GDAL_IMAGE
EOF
  exit 3
fi
need python3 "needed to write the GeoJSON files and meta.json"
need sha256sum "needed to check the downloads"
need unzip "needed to unpack the downloads"

# step CMD...: print it (--dry-run) or run it, stopping on failure.
step() {
  if [ $dry = 1 ]; then echo "would run: $*"; return; fi
  echo "make-overlays: $*"
  "$@" || die "failed: $*"
}
verify() { [ -f "$1" ] && printf '%s  %s\n' "$2" "$1" | sha256sum -c --status - 2>/dev/null; }

# ogr ARGS...: ogr2ogr with paths relative to the cache directory. The
# container writes as the calling user, so the next run can remove what it
# left: docker (and rootful podman) with --user; rootless podman, where the
# container's root already is the calling user, with --userns=keep-id.
userargs=()
if [ "$engine" = podman ] && [ "$(id -u)" != 0 ]; then userargs=(--userns=keep-id)
elif [ -n "$engine" ]; then userargs=(--user "$(id -u):$(id -g)"); fi
ogr() {
  if [ -n "$engine" ]; then
    step "$engine" run --rm --network=none ${userargs[@]+"${userargs[@]}"} -v "$cache:/c:Z" -w /c "$image" ogr2ogr "$@"
  elif [ $dry = 1 ]; then
    echo "would run (in $cache): ogr2ogr $*"
  else
    echo "make-overlays: (in $cache) ogr2ogr $*"
    (cd "$cache" && ogr2ogr "$@") || die "failed: ogr2ogr $*"
  fi
}

work=overlay-work          # in the cache
[ $dry = 1 ] || { mkdir -p "$cache" "$out"; rm -rf "${cache:?}/$work"; mkdir -p "$cache/$work"; }

for s in "${SOURCES[@]}"; do
  read -r name dir version sha <<<"$s"
  zip="$cache/$name.zip"
  if [ $dry = 1 ]; then
    if verify "$zip" "$sha"; then echo "cached: $zip (sha256 ok)"; else
      echo "would download: $NE_BASE/$dir/$name.zip -> $zip (sha256 $sha)"; fi
  elif verify "$zip" "$sha"; then
    echo "make-overlays: sha256 ok: $zip"
  else
    [ ! -e "$zip" ] || { rm -f "$zip"; echo "make-overlays: $zip did not match its pinned SHA-256; deleted it" >&2; }
    need curl "needed to download $name.zip"
    rm -f "$zip.part"
    curl -fL --retry 3 --connect-timeout 30 --max-time 600 --silent --show-error \
      -o "$zip.part" "$NE_BASE/$dir/$name.zip" || { rm -f "$zip.part"; die "download of $name.zip failed"; }
    verify "$zip.part" "$sha" || { rm -f "$zip.part"; die "$name.zip did not match its pinned SHA-256 (deleted)"; }
    mv "$zip.part" "$zip"
    echo "make-overlays: sha256 ok: $zip"
  fi
  # Each archive is unpacked into a directory of its own; only the
  # shapefile's parts are taken (.prj and .cpg when present), and nothing
  # from it is run.
  if [ $dry = 1 ]; then
    members=("$name.shp" "$name.shx" "$name.dbf" "$name.prj" "$name.cpg")
  else
    listing=$(unzip -Z1 "$zip") || die "cannot list $zip"
    members=()
    for ext in shp shx dbf prj cpg; do
      if grep -qxF "$name.$ext" <<<"$listing"; then members+=("$name.$ext")
      else case $ext in prj|cpg) ;; *) die "$zip has no $name.$ext" ;; esac; fi
    done
  fi
  step unzip -q -o -j "$zip" "${members[@]}" -d "$cache/$work/$name"
done

NA="('USA','CAN','MEX')"
ogr -f GeoJSON -simplify "$SIMPLIFY_DEG" "$work/admin0.geojson" \
  "$work/ne_50m_admin_0_boundary_lines_land/ne_50m_admin_0_boundary_lines_land.shp" \
  -sql "SELECT SCALERANK FROM ne_50m_admin_0_boundary_lines_land"
ogr -f GeoJSON -simplify "$SIMPLIFY_DEG" "$work/admin1.geojson" \
  "$work/ne_50m_admin_1_states_provinces_lines/ne_50m_admin_1_states_provinces_lines.shp" \
  -sql "SELECT SCALERANK FROM ne_50m_admin_1_states_provinces_lines WHERE ADM0_A3 IN $NA"
ogr -f GeoJSON "$work/cities.geojson" \
  "$work/ne_10m_populated_places_simple/ne_10m_populated_places_simple.shp" \
  -sql "SELECT name, scalerank, pop_max FROM ne_10m_populated_places_simple WHERE scalerank <= 7 OR (pop_max >= 50000 AND adm0_a3 IN $NA)"

generated=$(date -u +%Y-%m-%dT%H:%M:%SZ)
if [ $dry = 1 ]; then
  echo "would write: $out/admin0.json $out/admin1.json $out/cities.json $out/meta.json"
  exit 0
fi
# Compact GeoJSON: lines rounded to 3 decimals (about 100 m), cities to 4,
# properties only where the page uses them; then meta.json.
python3 -I - "$cache/$work" "$out" "$generated" "${SOURCES[@]}" <<'EOP' || die "failed to write $out"
import json, os, sys
work, out, generated, sources = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
src = {}
for s in sources:
    name, _dir, version, sha = s.split()
    src[name] = (version, sha)

def rnd(c, n):
    if isinstance(c[0], (int, float)):
        return [round(c[0], n), round(c[1], n)]
    return [rnd(x, n) for x in c]

def convert(kind, srcname):
    with open(os.path.join(work, kind + ".geojson"), encoding="utf-8") as f:
        g = json.load(f)
    feats = []
    for ft in g.get("features", []):
        geom = ft.get("geometry")
        if not geom or not geom.get("coordinates"):
            continue
        p = ft.get("properties") or {}
        if kind == "cities":
            if not p.get("name") or p.get("scalerank") is None:
                continue
            props = {"name": p["name"], "rank": int(p["scalerank"]), "pop": int(p.get("pop_max") or 0)}
            geom = {"type": "Point", "coordinates": rnd(geom["coordinates"], 4)}
        else:
            if geom.get("type") not in ("LineString", "MultiLineString"):
                continue
            props = {}
            geom = {"type": geom["type"], "coordinates": rnd(geom["coordinates"], 3)}
        feats.append({"type": "Feature", "properties": props, "geometry": geom})
    text = json.dumps({"type": "FeatureCollection", "features": feats}, ensure_ascii=False, separators=(",", ":"))
    path = os.path.join(out, kind + ".json")
    with open(path + ".part", "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(path + ".part", path)
    version, sha = src[srcname]
    return {"file": kind + ".json", "source": "Natural Earth " + srcname + " " + version,
            "sha256": sha, "features": len(feats), "bytes": len(text.encode("utf-8"))}

files = {
    "admin0": convert("admin0", "ne_50m_admin_0_boundary_lines_land"),
    "admin1": convert("admin1", "ne_50m_admin_1_states_provinces_lines"),
    "cities": convert("cities", "ne_10m_populated_places_simple"),
}
meta = {"files": files, "source": "Natural Earth (public domain), naturalearthdata.com", "generated": generated}
with open(os.path.join(out, "meta.json"), "w", encoding="utf-8") as f:
    json.dump(meta, f, indent=1)
    f.write("\n")
for k, v in files.items():
    print("make-overlays: %s.json  %d features, %d bytes" % (k, v["features"], v["bytes"]))
EOP
rm -rf "${cache:?}/$work"
echo "make-overlays: wrote $out ($(du -sh "$out" | cut -f1))"
