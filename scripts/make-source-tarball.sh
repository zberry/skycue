#!/usr/bin/env bash
# make-source-tarball.sh [-o OUTDIR]
# Writes OUTDIR/skycue-<version>.tar.gz (default OUTDIR: dist/): the committed
# tree (git archive HEAD) under skycue-<version>/ plus exactly the two pinned
# web-library tarballs from sources/ (leaflet, echarts). File names and SHA-256
# come from cmake/sources.cmake. Map tiles, overlays and every other file in
# sources/ are left out, as are the restricted test fixtures and the fuzz seeds
# cut from them. The output is reproducible for a given commit.
# The last line printed on stdout is the output path.
set -euo pipefail

die() { echo "make-source-tarball: $*" >&2; exit 1; }
usage() { echo "usage: $(basename "$0") [-o OUTDIR]" >&2; exit 2; }

repo=$(cd "$(dirname "$0")/.." && pwd)
outdir=$repo/dist
while [ $# -gt 0 ]; do
  case $1 in
    -o) [ $# -ge 2 ] || usage; outdir=$2; shift ;;
    *) usage ;;
  esac
  shift
done

version=$(sed -n 's/^project(skycue VERSION \([0-9.]*\).*/\1/p' "$repo/skycue/CMakeLists.txt")
[ -n "$version" ] || die "cannot read the version from CMakeLists.txt"
name=skycue-$version

# Name and hash of a pinned source, from its skycue_declare_source line.
pin() { # <source name> <field: SHA256|FILE>
  sed -n "s/^skycue_declare_source($1 .*$2 \"\\{0,1\\}\\([^ \"]*\\)\"\\{0,1\\}.*/\\1/p" "$repo/cmake/sources.cmake" | head -n1
}

files=()
for lib in leaflet echarts; do
  file=$(pin "$lib" FILE)
  sha=$(pin "$lib" SHA256)
  [ -n "$file" ] && [ -n "$sha" ] || die "no $lib pin in cmake/sources.cmake"
  [ -f "$repo/sources/$file" ] || die "sources/$file is missing"
  got=$(sha256sum "$repo/sources/$file" | cut -d' ' -f1)
  [ "$got" = "$sha" ] || die "sources/$file has the wrong SHA-256 (expected $sha, got $got)"
  files+=("$file")
done

if [ -n "$(git -C "$repo" status --porcelain --untracked-files=no)" ]; then
  echo "make-source-tarball: warning: uncommitted changes are not included (tarball is HEAD)" >&2
fi
git -C "$repo" describe --exact-match --tags --match "v$version" HEAD >/dev/null 2>&1 ||
  echo "make-source-tarball: warning: HEAD is not tagged v$version" >&2

mtime=$(git -C "$repo" log -1 --format=%ct HEAD)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

git -C "$repo" archive --format=tar --prefix="$name/" HEAD >"$tmp/src.tar"
# Left out of the public tarball: tests/fixtures/README.md says the
# third-party captures in fixtures/raw/ must not ship in release bundles, and
# the pingstation icd-*.json files are copied from a vendor document with no
# stated licence. The fuzz seeds cut from them (docs/hardening.md, "Seeds") go
# too: raw_formats/seed-1..18 and pingstation/seed-1, 3, 4 and web/seed-13.
# Only members the archive holds are deleted, so this keeps working once those
# files are gone from git.
excluded() { # reads tar -t names, prints the ones to delete
  local f=$name/skycue/tests/fixtures s=$name/skycue/fuzz/seeds
  grep -E "^($f/raw(/.*)?|$f/pingstation/icd-[^/]*\.json|$s/raw_formats/seed-([1-9]|1[0-8])\.[^/]*|$s/pingstation/seed-[134]\.[^/]*|$s/web/seed-13\.[^/]*)\$" || true
}
mapfile -t gone < <(tar -tf "$tmp/src.tar" | excluded)
[ ${#gone[@]} -eq 0 ] || tar --delete --no-recursion -f "$tmp/src.tar" "${gone[@]}"
for f in "${files[@]}"; do
  tar --append -f "$tmp/src.tar" -C "$repo" --transform "s,^sources/,$name/sources/," --format=gnu \
      --mtime="@$mtime" --mode=0644 --owner=0 --group=0 --numeric-owner "sources/$f"
done

mkdir -p "$outdir"
out=$(cd "$outdir" && pwd)/$name.tar.gz
gzip -n -9 <"$tmp/src.tar" >"$out"
echo "$out"
