#!/usr/bin/env bash
# Usage: scripts/fetch-test-captures.sh [--cache DIR] [--fixtures DIR] [--offline]
#
# Downloads the four third-party test captures the tests and the replay tools
# use from their upstream repositories, at pinned commits, checks them against
# pinned SHA-256 values and installs them in the (git-ignored) fixture paths:
#
#   tests/fixtures/raw/modes-120s.csv.gz           junzis/pyModeS (GPL-3.0)
#   tests/fixtures/raw/uat-sample.txt.gz           flightaware/dump978 (BSD-2-Clause)
#   tests/fixtures/raw/stratux-uat-2015.log.gz     cyoung/stratux (BSD-3-Clause),
#                                                  the upstream log, gzip -9n
#   tests/fixtures/fisb/tg30.978                   rand-projects/fisb-decode
#                                                  (BSD-2-Clause Plus Patent)
#
# It then regenerates tests/fixtures/fisb/received-with-uplinks.jsonl and
# received-tg30.jsonl from them (gen_received_*.py, python3 -I) and checks
# those against pinned SHA-256 values too. Provenance: tests/fixtures/README.md.
#
# --cache DIR     where the downloads are kept (default <repo>/build/test-captures)
# --fixtures DIR  the fixtures directory to fill (default <repo>/tests/fixtures);
#                 it must hold readsb-sample.jsonl, as the repository's does
# --offline       never download: use what is in the cache, and list the missing
#                 files with their URLs and SHA-256 values (to fetch by hand on
#                 a machine with network access and copy into the cache)
#
# Every step whose result already verifies is skipped, so a second run prints
# only "sha256 ok" lines and uses no network. A cache file with the wrong hash
# is deleted and the run fails; nothing is installed unless all four verify.
# The upstream hashes are strict; the gzip of the stratux log (made here) and
# the derived files are strict too, except the gzip hash, which only warns
# (another gzip build may compress differently; the content is still checked).
# Needs curl (only to download), python3, gzip and sha256sum.
#
# Tests may set SKYCUE_CAPTURE_URL_BASE (replaces the https://raw.githubusercontent.com
# prefix of every URL) and SKYCUE_CAPTURE_PINS=FILE (pin lines in the format
# below, instead of the block in this script).
# Exit status: 0 done, 1 failure (download, checksum, tool), 2 usage error.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)

# One line per file.
#   pin CACHEFILE SHA256 URL      an upstream file (strict)
#   gzip RELPATH SHA256           the installed gzip of the stratux log (warns)
#   derived RELPATH SHA256        a generated file under the fixtures directory
builtin_pins() {
  cat <<'PINS'
# --- pins ---
pin pyModeS-airsquitter_2026-07-13_120s.csv.gz fa433eb8dc2ec9cf6ebd40c1bf0a095ae753960d5e8c42ae6da15cc28eb3baea https://raw.githubusercontent.com/junzis/pyModeS/487cfd6139ecea10a833afea2e58f631dc875702/scripts/data/airsquitter_2026-07-13_120s.csv.gz
pin dump978-sample-data.txt.gz 86248797f4334a134ccf4349dabf800a7054d698828b88838dc7950a99f9ae24 https://raw.githubusercontent.com/flightaware/dump978/f40aa9a0d2d0a0067c0c628b44ba316f7049b8ce/sample-data.txt.gz
pin stratux-cyoung-09062015-noproblem-stratux-uat.log fd4b376c7d90fe92dd1f9d5f2f8d11a19deb81ea3dc68e9b6e10f091e33d73f5 https://raw.githubusercontent.com/cyoung/stratux/1a0d1ca9e94d1c98ae1f4305e073fdd6b5a810be/test-data/cyoung-09062015-noproblem-stratux-uat.log
pin fisb-decode-tg30.978 6df9e5671b501e4317bc7f7b661e3931bf461d47f3666e853457e7f6abd45583 https://raw.githubusercontent.com/rand-projects/fisb-decode/c3d3b0473429336b1a98672b2e8b276c24d49765/tg/tg-source/generated/tg30.978
gzip raw/stratux-uat-2015.log.gz 53fc80938c55d777ef85929fbaef519a7ccfa41ecb37c6e23aa773daebe5d588
derived fisb/received-with-uplinks.jsonl 2ee0a8e9805bf6f0afa14121d9807f8f5ed8f69a312253c9d05e22a36a4ca346
derived fisb/received-tg30.jsonl 95633adbe73d449c2904d41b7b65d80e1735d3f4101b4a07df3bf38fe486a399
# --- end pins ---
PINS
}

usage() {
  echo "usage: $0 [--cache DIR] [--fixtures DIR] [--offline]" >&2
  exit 2
}

cache=$here/build/test-captures
fixtures=$here/tests/fixtures
offline=0
while [ $# -gt 0 ]; do
  case $1 in
    --cache)
      [ $# -ge 2 ] && [ -n "$2" ] || usage
      cache=$2
      shift 2
      ;;
    --fixtures)
      [ $# -ge 2 ] && [ -n "$2" ] || usage
      fixtures=$2
      shift 2
      ;;
    --offline)
      offline=1
      shift
      ;;
    -h | --help)
      sed -n '2,35p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) usage ;;
  esac
done

die() {
  echo "fetch-test-captures.sh: $*" >&2
  exit 1
}

# verify FILE SHA: 0 when FILE exists and has that SHA-256.
verify() {
  [ -f "$1" ] && printf '%s  %s\n' "$2" "$1" | sha256sum -c --status - 2>/dev/null
}

if [ -n "${SKYCUE_CAPTURE_PINS:-}" ]; then
  [ -f "$SKYCUE_CAPTURE_PINS" ] || die "SKYCUE_CAPTURE_PINS: no such file: $SKYCUE_CAPTURE_PINS"
  pins=$(grep -E '^(pin|gzip|derived) ' "$SKYCUE_CAPTURE_PINS" || true)
else
  pins=$(builtin_pins | grep -E '^(pin|gzip|derived) ' || true)
fi

# pin_sha KIND NAME: the SHA-256 on the KIND line for NAME.
pin_sha() {
  local k n s
  while read -r k n s _; do
    if [ "$k" = "$1" ] && [ "$n" = "$2" ]; then
      echo "$s"
      return 0
    fi
  done <<<"$pins"
  die "no '$1 $2' line in the pins"
}

# install_path CACHEFILE: where a cache file goes under the fixtures directory.
install_path() {
  case $1 in
    pyModeS-airsquitter_*) echo raw/modes-120s.csv.gz ;;
    dump978-sample-data.txt.gz) echo raw/uat-sample.txt.gz ;;
    stratux-*) echo raw/stratux-uat-2015.log.gz ;;
    fisb-decode-tg30.978) echo fisb/tg30.978 ;;
    *) die "no install path for cache file $1" ;;
  esac
}

mkdir -p "$cache"
# A *.part file is an interrupted download: never trusted.
rm -f "$cache"/*.part

# --- 1. the cache: verify, delete the bad, download the missing ---
failed=0
missing=""
while read -r kind name sha url; do
  [ "$kind" = pin ] || continue
  f=$cache/$name
  if verify "$f" "$sha"; then
    echo "sha256 ok: $f"
    continue
  fi
  if [ -e "$f" ]; then
    rm -f "$f"
    echo "fetch-test-captures.sh: $f did not match its pinned SHA-256; deleted it" >&2
    failed=1
    continue
  fi
  if [ -n "${SKYCUE_CAPTURE_URL_BASE:-}" ]; then
    url=${SKYCUE_CAPTURE_URL_BASE}${url#https://raw.githubusercontent.com}
  fi
  if [ "$offline" -eq 1 ]; then
    missing="$missing
  $name
    SHA-256 $sha
    $url"
    failed=1
    continue
  fi
  command -v curl >/dev/null || die "curl is needed to download $url"
  echo "downloading $url"
  if ! curl -fL --retry 3 --connect-timeout 30 --max-time 900 --silent --show-error -o "$f.part" "$url"; then
    rm -f "$f.part"
    echo "fetch-test-captures.sh: download of $url failed" >&2
    failed=1
    continue
  fi
  if ! verify "$f.part" "$sha"; then
    rm -f "$f.part"
    echo "fetch-test-captures.sh: the download of $url did not match its pinned SHA-256; deleted it" >&2
    failed=1
    continue
  fi
  mv "$f.part" "$f"
  echo "sha256 ok: $f"
done <<<"$pins"

if [ -n "$missing" ]; then
  {
    echo "fetch-test-captures.sh: --offline and these files are not in $cache."
    echo "Download each from its URL into that directory under the name shown:$missing"
  } >&2
fi
[ "$failed" -eq 0 ] || die "nothing was installed"

# --- 2. install the verified files ---
command -v python3 >/dev/null || die "python3 is needed to regenerate the derived fixtures"
command -v gzip >/dev/null || die "gzip is needed for the stratux log"
mkdir -p "$fixtures/raw" "$fixtures/fisb"

while read -r kind name sha url; do
  [ "$kind" = pin ] || continue
  rel=$(install_path "$name")
  dest=$fixtures/$rel
  src=$cache/$name
  if [ "$rel" = raw/stratux-uat-2015.log.gz ]; then
    # The installed file is the upstream log, gzip -9n; its content is what is
    # pinned strictly, the compressed bytes only warn.
    if [ -f "$dest" ] && [ "$(gzip -dc "$dest" 2>/dev/null | sha256sum | cut -d' ' -f1)" = "$sha" ]; then
      echo "sha256 ok: $dest"
      continue
    fi
    rm -f "$dest" "$dest.part"
    gzip -9n <"$src" >"$dest.part" || die "gzip of $src failed"
    mv "$dest.part" "$dest"
    gz_sha=$(pin_sha gzip "$rel")
    if ! verify "$dest" "$gz_sha"; then
      echo "fetch-test-captures.sh: warning: $dest has SHA-256 $(sha256sum "$dest" | cut -d' ' -f1)," >&2
      echo "  not the pinned $gz_sha: this gzip compresses differently. The log inside matches; keeping it." >&2
    fi
    echo "sha256 ok: $dest (gunzips to the pinned log)"
    continue
  fi
  if verify "$dest" "$sha"; then
    echo "sha256 ok: $dest"
    continue
  fi
  rm -f "$dest" "$dest.part"
  cp "$src" "$dest.part"
  mv "$dest.part" "$dest"
  echo "sha256 ok: $dest"
done <<<"$pins"

# --- 3. the derived files ---
gen() { # gen SCRIPT: run a generator against the fixtures directory
  python3 -I "$here/tests/fixtures/fisb/$1" --fixtures "$fixtures" >/dev/null || die "$1 failed"
}
while read -r kind rel sha _; do
  [ "$kind" = derived ] || continue
  dest=$fixtures/$rel
  if verify "$dest" "$sha"; then
    echo "sha256 ok: $dest"
    continue
  fi
  rm -f "$dest"
  case $rel in
    fisb/received-with-uplinks.jsonl)
      [ -f "$fixtures/readsb-sample.jsonl" ] || die "$fixtures/readsb-sample.jsonl is needed to generate $rel (copy it from the repository's tests/fixtures)"
      gen gen_received_with_uplinks.py
      ;;
    fisb/received-tg30.jsonl) gen gen_received_tg30.py ;;
    *) die "no generator for $rel" ;;
  esac
  if ! verify "$dest" "$sha"; then
    actual=$(sha256sum "$dest" | cut -d' ' -f1)
    rm -f "$dest"
    die "the generated $rel has SHA-256 $actual, not the pinned $sha; deleted it"
  fi
  echo "sha256 ok: $dest"
done <<<"$pins"
