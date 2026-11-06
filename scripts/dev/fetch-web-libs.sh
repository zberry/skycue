#!/usr/bin/env bash
# Usage: scripts/dev/fetch-web-libs.sh [--dest DIR] [--offline] [--declarations FILE]
#
# Puts the two web libraries the page needs (Leaflet and ECharts, as npm
# tarballs) into <repo>/sources/, where a build from a git clone looks for
# them (the release tarball already carries them). The file names, URLs and
# SHA-256 values are read at run time from the skycue_declare_source() lines
# in cmake/web-sources.cmake (or FILE, with --declarations), so this script
# never has its own copy of them.
#
#   --dest DIR      write into DIR instead of <repo>/sources
#   --offline       never download: only check what is already in DIR, and
#                   say what is missing (for a machine without network
#                   access: copy the named files into DIR by hand)
#   --declarations  read the declarations from FILE
#
# Every tarball already in DIR that matches its pinned SHA-256 is left alone,
# so a second run downloads nothing. A file that does not match is deleted and
# the script fails. Needs curl (only to download) and sha256sum.
# Exit status: 0 done, 1 failure (download, checksum, missing file), 2 usage error.
set -euo pipefail

here=$(cd "$(dirname "$0")/../.." && pwd)

usage() {
  echo "usage: $0 [--dest DIR] [--offline] [--declarations FILE]" >&2
  exit 2
}

dest=$here/sources
decl=$here/cmake/web-sources.cmake
offline=0
while [ $# -gt 0 ]; do
  case $1 in
    --dest)
      [ $# -ge 2 ] && [ -n "$2" ] || usage
      dest=$2
      shift 2
      ;;
    --declarations)
      [ $# -ge 2 ] && [ -n "$2" ] || usage
      decl=$2
      shift 2
      ;;
    --offline)
      offline=1
      shift
      ;;
    -h | --help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) usage ;;
  esac
done

die() {
  echo "fetch-web-libs.sh: $*" >&2
  exit 1
}

[ -f "$decl" ] || die "$decl not found"

# verify FILE SHA: 0 when FILE exists and has that SHA-256.
verify() {
  [ -f "$1" ] && printf '%s  %s\n' "$2" "$1" | sha256sum -c --status - 2>/dev/null
}

# One "URL SHA256 FILE" line per declaration.
entries=$(sed -n 's/^skycue_declare_source([^ ]* URL "\([^"]*\)" SHA256 \([0-9a-f]\{64\}\) FILE "\([^"]*\)").*$/\1 \2 \3/p' "$decl")
[ -n "$entries" ] || die "no skycue_declare_source(... URL ... SHA256 ... FILE ...) line in $decl"

mkdir -p "$dest"
missing=0
while read -r url sha name; do
  file=$dest/$name
  if verify "$file" "$sha"; then
    echo "sha256 ok: $file"
    continue
  fi
  if [ -e "$file" ]; then
    rm -f "$file"
    die "$file did not match its pinned SHA-256; deleted it"
  fi
  if [ "$offline" -eq 1 ]; then
    echo "missing: $name: download $url into $dest (SHA-256 $sha)" >&2
    missing=1
    continue
  fi
  command -v curl >/dev/null || die "curl is needed to download $url"
  echo "downloading $url"
  rm -f "$file.part"
  curl -fL --retry 3 --connect-timeout 30 --max-time 300 --silent --show-error -o "$file.part" "$url" || {
    rm -f "$file.part"
    die "download of $url failed"
  }
  if ! verify "$file.part" "$sha"; then
    rm -f "$file.part"
    die "the download of $name did not match its pinned SHA-256; deleted it"
  fi
  mv "$file.part" "$file"
  echo "sha256 ok: $file"
done <<<"$entries"

[ "$missing" -eq 0 ] || die "files are missing (--offline given)"
