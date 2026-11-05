#!/usr/bin/env bash
# make-source-tarball.sh [-o OUTDIR]
# Writes OUTDIR/skycue-<version>.tar.gz (default OUTDIR: dist/): the committed
# tree (git archive HEAD) directly under skycue-<version>/ plus exactly the two
# pinned web-library tarballs from sources/ (leaflet, echarts). File names and
# SHA-256 come from cmake/web-sources.cmake. Map tiles, overlays and every other
# file in sources/ are left out. The archive is the tree: nothing is excluded.
# Works whether this directory is a subdirectory of a git repository or its root.
# The output is reproducible for a given commit.
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

version=$(sed -n 's/^project(skycue VERSION \([0-9.]*\).*/\1/p' "$repo/CMakeLists.txt")
[ -n "$version" ] || die "cannot read the version from CMakeLists.txt"
name=skycue-$version

# Name and hash of a pinned source, from its skycue_declare_source line.
pin() { # <source name> <field: SHA256|FILE>
  sed -n "s/^skycue_declare_source($1 .*$2 \"\\{0,1\\}\\([^ \"]*\\)\"\\{0,1\\}.*/\\1/p" "$repo/cmake/web-sources.cmake" | head -n1
}

files=()
for lib in leaflet echarts; do
  file=$(pin "$lib" FILE)
  sha=$(pin "$lib" SHA256)
  [ -n "$file" ] && [ -n "$sha" ] || die "no $lib pin in cmake/web-sources.cmake"
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

prefix=$(git -C "$repo" rev-parse --show-prefix)
# From the top level: inside a subdirectory git archive would filter by the cwd again.
# A tree-ish has no commit time, so --mtime (git 2.39 or newer) pins it.
git -C "$(git -C "$repo" rev-parse --show-toplevel)" archive --format=tar --mtime="@$mtime" --prefix="$name/" "HEAD:$prefix" >"$tmp/src.tar"
for f in "${files[@]}"; do
  tar --append -f "$tmp/src.tar" -C "$repo" --transform "s,^sources/,$name/sources/," --format=gnu \
      --mtime="@$mtime" --mode=0644 --owner=0 --group=0 --numeric-owner "sources/$f"
done

mkdir -p "$outdir"
out=$(cd "$outdir" && pwd)/$name.tar.gz
gzip -n -9 <"$tmp/src.tar" >"$out"
echo "$out"
