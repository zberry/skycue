#!/usr/bin/env bash
# build-reference-decoders.sh: build the two reference decoders the test-data tests
# compare against, from pinned upstream tarballs.
#
# usage: build-reference-decoders.sh DIR
#        build-reference-decoders.sh --help
#
# Builds readsb and dump978-fa under DIR/src and installs them as DIR/bin/readsb and
# DIR/bin/dump978-fa. The last two lines printed are
#   readsb=<absolute path>
#   dump978=<absolute path>
# The tests reach the decoders only through SKYCUE_READSB, SKYCUE_DUMP978 and (at
# configure time) SKYCUE_TEST_READSB; nothing else in the tree uses them.
#
# Pins (tarball and SHA-256):
#   readsb   https://github.com/wiedehopf/readsb/archive/refs/tags/v3.16.21.tar.gz
#            b89c0b9a241384b709a9febfea61533169f4e7b3be5f01b99810e36b751c0a03
#   dump978  https://github.com/flightaware/dump978/archive/f40aa9a0d2d0a0067c0c628b44ba316f7049b8ce.tar.gz
#            c2f4acd912c6117dbd17e44dc380df6e4acc8cfa5e071d426988b447fadc205a
#
# Build dependencies on Debian and Ubuntu:
#   build-essential libncurses-dev zlib1g-dev libzstd-dev libboost-system-dev
#   libboost-program-options-dev libboost-regex-dev libboost-filesystem-dev
#   libsoapysdr-dev
#
# Environment:
#   SKYCUE_REFERENCE_JOBS      parallel make jobs (default: nproc)
#   SKYCUE_REFERENCE_OFFLINE   1: copy readsb.tar.gz and dump978.tar.gz from
#                              SKYCUE_REFERENCE_SRC_DIR instead of downloading (for tests)
#   SKYCUE_REFERENCE_SRC_DIR   directory holding those two tarballs
#
# Exit status: 0 built, 1 failed (download, digest or build), 2 usage.
set -euo pipefail

readsb_url=https://github.com/wiedehopf/readsb/archive/refs/tags/v3.16.21.tar.gz
readsb_sha=b89c0b9a241384b709a9febfea61533169f4e7b3be5f01b99810e36b751c0a03
dump978_url=https://github.com/flightaware/dump978/archive/f40aa9a0d2d0a0067c0c628b44ba316f7049b8ce.tar.gz
dump978_sha=c2f4acd912c6117dbd17e44dc380df6e4acc8cfa5e071d426988b447fadc205a

me=build-reference-decoders
usage() { echo "usage: $0 DIR" >&2; exit 2; }

if [ "${1:-}" = "--help" ]; then
  cat <<EOT
usage: $0 DIR
Builds readsb and dump978-fa into DIR/bin from pinned tarballs.
  readsb   $readsb_url
           sha256 $readsb_sha
  dump978  $dump978_url
           sha256 $dump978_sha
EOT
  exit 0
fi
[ "$#" -eq 1 ] || usage

dir=$1
mkdir -p "$dir/src"
dir=$(cd "$dir" && pwd)
jobs=${SKYCUE_REFERENCE_JOBS:-$(nproc)}

# fetch NAME URL SHA: put the verified tarball at DIR/src/NAME.tar.gz.
fetch() {
  local name=$1 url=$2 sha=$3 tgz="$dir/src/$1.tar.gz" got
  if [ "${SKYCUE_REFERENCE_OFFLINE:-}" = 1 ]; then
    cp "${SKYCUE_REFERENCE_SRC_DIR:?SKYCUE_REFERENCE_SRC_DIR is required offline}/$name.tar.gz" "$tgz"
  else
    curl -fsSL -o "$tgz" "$url"
  fi
  got=$(sha256sum "$tgz" | cut -d' ' -f1)
  if [ "$got" != "$sha" ]; then
    echo "$me: $name: digest mismatch" >&2
    exit 1
  fi
  rm -rf "${dir:?}/src/$name"
  mkdir -p "$dir/src/$name"
  tar -xzf "$tgz" -C "$dir/src/$name" --strip-components=1
}

fetch readsb "$readsb_url" "$readsb_sha"
fetch dump978 "$dump978_url" "$dump978_sha"

mkdir -p "$dir/bin"

(
  cd "$dir/src/readsb"
  sed -i -e '/^CFLAGS = /s/ -Werror / /' Makefile
  make -j"$jobs" RTLSDR=no BLADERF=no HACKRF=no PLUTOSDR=no SOAPYSDR=no
)
cp "$dir/src/readsb/readsb" "$dir/bin/readsb"

(
  cd "$dir/src/dump978"
  sed -i -e 's/ -Werror//' Makefile
  make -j"$jobs" VERSION=f40aa9a dump978-fa
)
cp "$dir/src/dump978/dump978-fa" "$dir/bin/dump978-fa"

echo "readsb=$dir/bin/readsb"
echo "dump978=$dir/bin/dump978-fa"
