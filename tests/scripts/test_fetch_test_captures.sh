#!/usr/bin/env bash
# fetch-test-captures.sh: pinned third-party captures into git-ignored test data paths
# paths. Uses small synthetic captures served over file:// URLs through
# SKYCUE_CAPTURE_URL_BASE and a pins file written here, so it needs neither the
# network nor the real captures. (The real pins are checked by a real run.)
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
script="$here/scripts/test-data/fetch-test-captures.sh"
gens="$here/tests/data/fisb"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
out="$tmp/out"
fail() { echo "FAIL: test_fetch_test_captures: $*"; [ -f "$out" ] && cat "$out"; exit 1; }
command -v python3 >/dev/null || { echo "SKIP test_fetch_test_captures: no python3"; exit 77; }
command -v curl >/dev/null || { echo "SKIP test_fetch_test_captures: no curl"; exit 77; }
[ -f "$script" ] || fail "scripts/test-data/fetch-test-captures.sh missing"

sha() { sha256sum "$1" | cut -d' ' -f1; }

# --- synthetic upstream captures ---
up=$tmp/up
mkdir -p "$up/o/modes/c1" "$up/o/uat/c2" "$up/o/tg/c3" "$up/o/stx/c4"
printf '1783963099.005369,5F406A936F50F0\n1783963099.100000,8D4840D6202CC371C32CE0576098\n' |
  gzip -9n >"$up/o/modes/c1/modes.csv.gz"
printf -- '-00a66ef135445d525a0c0519119021204800;\n+%s;\n+%s;\n+%s;\n' \
  "$(printf 'a1%.0s' $(seq 1 432))" "$(printf 'b2%.0s' $(seq 1 432))" "$(printf 'c3%.0s' $(seq 1 432))" |
  gzip -9n >"$up/o/uat/c2/uat.txt.gz"
{
  echo '#  RECENTER 15 -> 1767 i:1167'
  printf '+%s;rs=2;rssi=-9.5;t=1604047960.123;\n' "$(printf 'd4%.0s' $(seq 1 432))"
  printf '+%s;rs=3;rssi=-8.0;t=1604047961;\n' "$(printf 'e5%.0s' $(seq 1 432))"
} >"$up/o/tg/c3/tg30.978"
{
  echo 'START,Wed Sep 2 04:23:37 +0000 UTC 2015'
  echo '162698355928,-0b2b4a513c417b8875f40d9610f227401105c4e6c4e6c40a32700300000000000000;rs=4;'
  echo
} >"$up/o/stx/c4/stratux.log"
# readsb-sample.jsonl is a repository file the generator reads, not downloaded.
readsb=$tmp/readsb-sample.jsonl
for i in 0 10 20 29 31 40; do
  printf '{"now":%d.500,"messages":%d,"aircraft":[]}\n' $((1700000000 + i)) "$i"
done >"$readsb"

url_base=file://$up
M=modes-cache.csv.gz
U=uat-cache.txt.gz
S=stratux-cache.log
T=tg30-cache.978
# Real URLs have the form https://raw.githubusercontent.com/..., the override
# replaces that prefix.
raw=https://raw.githubusercontent.com
cat >"$tmp/pins.txt" <<EOF
pin pyModeS-airsquitter_2026-07-13_120s.csv.gz $(sha "$up/o/modes/c1/modes.csv.gz") $raw/o/modes/c1/modes.csv.gz
pin dump978-sample-data.txt.gz $(sha "$up/o/uat/c2/uat.txt.gz") $raw/o/uat/c2/uat.txt.gz
pin stratux-cyoung-09062015-noproblem-stratux-uat.log $(sha "$up/o/stx/c4/stratux.log") $raw/o/stx/c4/stratux.log
pin fisb-decode-tg30.978 $(sha "$up/o/tg/c3/tg30.978") $raw/o/tg/c3/tg30.978
EOF
cache_names="pyModeS-airsquitter_2026-07-13_120s.csv.gz dump978-sample-data.txt.gz stratux-cyoung-09062015-noproblem-stratux-uat.log fisb-decode-tg30.978"

# Reference test data tree: what the generators make from these inputs, and the
# derived pins computed from it.
ref=$tmp/ref
mkdir -p "$ref/raw" "$ref/fisb"
cp "$up/o/uat/c2/uat.txt.gz" "$ref/raw/uat-sample.txt.gz"
cp "$up/o/tg/c3/tg30.978" "$ref/fisb/tg30.978"
cp "$readsb" "$ref/readsb-sample.jsonl"
python3 -I "$gens/gen_received_with_uplinks.py" --test-data "$ref" >/dev/null || fail "generator 1 on the reference tree"
python3 -I "$gens/gen_received_tg30.py" --test-data "$ref" >/dev/null || fail "generator 2 on the reference tree"
rw=$(sha "$ref/fisb/received-with-uplinks.jsonl")
rt=$(sha "$ref/fisb/received-tg30.jsonl")
[ "$(wc -l <"$ref/fisb/received-with-uplinks.jsonl")" -eq 7 ] || fail "reference received-with-uplinks.jsonl should have 3 uplinks + 4 readsb lines"
[ "$(wc -l <"$ref/fisb/received-tg30.jsonl")" -eq 2 ] || fail "reference received-tg30.jsonl should have 2 uplinks"
stx_gz=$tmp/stx.gz
gzip -9n <"$up/o/stx/c4/stratux.log" >"$stx_gz"
cat >>"$tmp/pins.txt" <<EOF
gzip raw/stratux-uat-2015.log.gz $(sha "$stx_gz")
derived fisb/received-with-uplinks.jsonl $rw
derived fisb/received-tg30.jsonl $rt
EOF

export SKYCUE_CAPTURE_PINS=$tmp/pins.txt
export SKYCUE_CAPTURE_URL_BASE=$url_base
run() { "$script" "$@" >"$out" 2>&1; rc=$?; }

mkdir_td() { mkdir -p "$1"; cp "$readsb" "$1/readsb-sample.jsonl"; }
fill_cache() {
  mkdir -p "$1"
  cp "$up/o/modes/c1/modes.csv.gz" "$1/pyModeS-airsquitter_2026-07-13_120s.csv.gz"
  cp "$up/o/uat/c2/uat.txt.gz" "$1/dump978-sample-data.txt.gz"
  cp "$up/o/stx/c4/stratux.log" "$1/stratux-cyoung-09062015-noproblem-stratux-uat.log"
  cp "$up/o/tg/c3/tg30.978" "$1/fisb-decode-tg30.978"
}
installed="raw/modes-120s.csv.gz raw/uat-sample.txt.gz raw/stratux-uat-2015.log.gz fisb/tg30.978 fisb/received-with-uplinks.jsonl fisb/received-tg30.jsonl"

# (a) --offline with an empty cache: exit 1, all four missing files named, with
# their URLs, and no download attempt (the URL base points nowhere).
mkdir_td "$tmp/tda"
SKYCUE_CAPTURE_URL_BASE=file://$tmp/nowhere run --cache "$tmp/ca" --test-data "$tmp/tda" --offline
[ "$rc" -eq 1 ] || fail "(a) --offline with an empty cache exited $rc, want 1"
for n in $cache_names; do grep -q -- "$n" "$out" || fail "(a) output does not name $n"; done
grep -q "file://$tmp/nowhere/o/modes/c1/modes.csv.gz" "$out" || fail "(a) output does not list the (overridden) URL"
[ ! -e "$tmp/tda/raw" ] || fail "(a) installed something"

# (a2) the same without the test overrides: the script's own pins. --offline
# with an empty cache lists every built-in URL and SHA-256, and downloads nothing.
# The pin count is not fixed: every "pin " line the script declares must be
# well formed (name, 64-hex SHA-256, https URL), the four original cache names
# must be among them, and the run must list each one. SKYCUE_FETCH_IQ=1 lifts
# the opt-in skip of the zenodo-* pin so that it is listed as well.
env -u SKYCUE_CAPTURE_PINS -u SKYCUE_CAPTURE_URL_BASE SKYCUE_FETCH_IQ=1 "$script" --cache "$tmp/ca2" --test-data "$tmp/tda2" --offline >"$out" 2>&1
rc=$?
[ "$rc" -eq 1 ] || fail "(a2) --offline with the built-in pins and an empty cache exited $rc, want 1"
declared=$(grep -c '^pin ' "$script")
builtin=$(grep -E '^pin [^ ]+ [0-9a-f]{64} https://' "$script")
[ "$declared" -ge 4 ] || fail "(a2) the script declares fewer than four pins"
[ "$(printf '%s\n' "$builtin" | grep -c .)" -eq "$declared" ] || fail "(a2) a declared pin line is malformed (want a name, a 64-hex SHA-256 and an https URL)"
for n in $cache_names; do printf '%s\n' "$builtin" | grep -q "^pin $n " || fail "(a2) the built-in pins lack $n"; done
while read -r _ name hash url; do
  grep -q -- "$name" "$out" || fail "(a2) output does not name $name"
  grep -q -- "SHA-256 $hash" "$out" || fail "(a2) output does not list the SHA-256 of $name"
  grep -qF -- "$url" "$out" || fail "(a2) output does not list the URL of $name"
done <<<"$builtin"
[ ! -e "$tmp/tda2/raw" ] && [ ! -e "$tmp/ca2/fisb-decode-tg30.978" ] || fail "(a2) installed or downloaded something"
# (a2) without SKYCUE_FETCH_IQ=1 the zenodo-* pin is skipped, not listed.
env -u SKYCUE_CAPTURE_PINS -u SKYCUE_CAPTURE_URL_BASE -u SKYCUE_FETCH_IQ "$script" --cache "$tmp/ca3" --test-data "$tmp/tda3" --offline >"$out" 2>&1
[ "$?" -eq 1 ] || fail "(a2) --offline without SKYCUE_FETCH_IQ exited other than 1"
! grep -q 'zenodo-' "$out" || fail "(a2) the zenodo-* pin was listed without SKYCUE_FETCH_IQ=1"

# (a3) -h prints the whole header comment: its first line and its last.
run -h
[ "$rc" -eq 0 ] || fail "(a3) -h exited $rc, want 0"
grep -q '^Usage: scripts/test-data/fetch-test-captures.sh' "$out" || fail "(a3) -h lacks the usage line"
grep -q '^Exit status: 0 done, 1 failure' "$out" || fail "(a3) -h lacks the exit status line (the header is cut)"
! grep -q 'set -euo pipefail' "$out" || fail "(a3) -h prints code"

# (b) usage errors exit 2.
run --bogus
[ "$rc" -eq 2 ] || fail "(b) --bogus exited $rc, want 2"
run --cache
[ "$rc" -eq 2 ] || fail "(b) --cache without a value exited $rc, want 2"
run --test-data ""
[ "$rc" -eq 2 ] || fail "(b) --test-data with an empty value exited $rc, want 2"

# (c) a full cache installs offline; derived files match the pins; the second
# run only prints "sha256 ok" lines.
fill_cache "$tmp/cc"
mkdir_td "$tmp/tdc"
run --cache "$tmp/cc" --test-data "$tmp/tdc" --offline
[ "$rc" -eq 0 ] || fail "(c) exited $rc, want 0"
for f in $installed; do [ -s "$tmp/tdc/$f" ] || fail "(c) $f not installed"; done
[ "$(sha "$tmp/tdc/fisb/received-with-uplinks.jsonl")" = "$rw" ] || fail "(c) received-with-uplinks.jsonl hash"
[ "$(sha "$tmp/tdc/fisb/received-tg30.jsonl")" = "$rt" ] || fail "(c) received-tg30.jsonl hash"
cmp -s "$tmp/tdc/raw/modes-120s.csv.gz" "$up/o/modes/c1/modes.csv.gz" || fail "(c) modes copy differs"
cmp -s "$tmp/tdc/raw/uat-sample.txt.gz" "$up/o/uat/c2/uat.txt.gz" || fail "(c) uat copy differs"
cmp -s "$tmp/tdc/fisb/tg30.978" "$up/o/tg/c3/tg30.978" || fail "(c) tg30 copy differs"
gzip -dc "$tmp/tdc/raw/stratux-uat-2015.log.gz" | cmp -s - "$up/o/stx/c4/stratux.log" || fail "(c) stratux gz does not decompress to the upstream log"
[ "$(sha "$tmp/tdc/raw/stratux-uat-2015.log.gz")" = "$(sha "$stx_gz")" ] || fail "(c) stratux gz is not gzip -9n"
run --cache "$tmp/cc" --test-data "$tmp/tdc" --offline
[ "$rc" -eq 0 ] || fail "(c) second run exited $rc"
[ -s "$out" ] || fail "(c) second run printed nothing"
if grep -qv '^sha256 ok' "$out"; then fail "(c) second run printed something besides sha256 ok lines"; fi
before=$(cd "$tmp/tdc" && sha256sum $installed)
run --cache "$tmp/cc" --test-data "$tmp/tdc" --offline
[ "$before" = "$(cd "$tmp/tdc" && sha256sum $installed)" ] || fail "(c) third run changed files"

# (c2) a damaged derived file is regenerated.
echo junk >"$tmp/tdc/fisb/received-tg30.jsonl"
run --cache "$tmp/cc" --test-data "$tmp/tdc" --offline
[ "$rc" -eq 0 ] && [ "$(sha "$tmp/tdc/fisb/received-tg30.jsonl")" = "$rt" ] || fail "(c2) damaged derived file not regenerated"

# (d) a truncated cache file: exit 1, the file deleted, nothing installed.
fill_cache "$tmp/cd"
mkdir_td "$tmp/tdd"
head -c 5 "$up/o/tg/c3/tg30.978" >"$tmp/cd/fisb-decode-tg30.978"
run --cache "$tmp/cd" --test-data "$tmp/tdd" --offline
[ "$rc" -eq 1 ] || fail "(d) exited $rc, want 1"
[ ! -e "$tmp/cd/fisb-decode-tg30.978" ] || fail "(d) the truncated file was not deleted"
grep -q fisb-decode-tg30.978 "$out" || fail "(d) output does not name the bad file"
[ ! -e "$tmp/tdd/raw" ] && [ ! -e "$tmp/tdd/fisb" ] || fail "(d) something was installed"

# (e) a *.part file in the cache is ignored and removed.
fill_cache "$tmp/ce"
mkdir_td "$tmp/tde"
echo partial >"$tmp/ce/fisb-decode-tg30.978.part"
echo partial >"$tmp/ce/other.part"
run --cache "$tmp/ce" --test-data "$tmp/tde" --offline
[ "$rc" -eq 0 ] || fail "(e) exited $rc, want 0"
[ ! -e "$tmp/ce/fisb-decode-tg30.978.part" ] && [ ! -e "$tmp/ce/other.part" ] || fail "(e) a .part file was left"

# (f) downloading (file:// URLs): an empty cache fills, a second run downloads
# nothing (the upstream is gone) and prints only sha256 ok lines.
mkdir_td "$tmp/tdf"
run --cache "$tmp/cf" --test-data "$tmp/tdf"
[ "$rc" -eq 0 ] || fail "(f) exited $rc, want 0"
for n in $cache_names; do [ -s "$tmp/cf/$n" ] || fail "(f) $n not cached"; done
for f in $installed; do [ -s "$tmp/tdf/$f" ] || fail "(f) $f not installed"; done
[ "$(sha "$tmp/tdf/fisb/received-with-uplinks.jsonl")" = "$rw" ] || fail "(f) derived hash"
mv "$up" "$tmp/up-gone"
run --cache "$tmp/cf" --test-data "$tmp/tdf"
[ "$rc" -eq 0 ] || fail "(f) second run exited $rc"
if grep -qv '^sha256 ok' "$out"; then fail "(f) second run printed something besides sha256 ok lines"; fi
mv "$tmp/up-gone" "$up"

# (g) a download that does not match its pin: exit 1, nothing kept or installed.
mkdir_td "$tmp/tdg"
cp "$up/o/uat/c2/uat.txt.gz" "$tmp/uat.orig"
echo tampered >>"$up/o/uat/c2/uat.txt.gz"
run --cache "$tmp/cg" --test-data "$tmp/tdg"
[ "$rc" -eq 1 ] || fail "(g) exited $rc, want 1"
[ ! -e "$tmp/cg/dump978-sample-data.txt.gz" ] && ! ls "$tmp/cg"/*.part >/dev/null 2>&1 || fail "(g) the bad download was kept"
[ ! -e "$tmp/tdg/raw/uat-sample.txt.gz" ] || fail "(g) the bad download was installed"
cp "$tmp/uat.orig" "$up/o/uat/c2/uat.txt.gz"

# (h) a missing readsb-sample.jsonl in a non-default test data directory is a clear
# failure for the derived file, not a traceback.
fill_cache "$tmp/ch"
rm -rf "$tmp/tdh"
mkdir -p "$tmp/tdh"
run --cache "$tmp/ch" --test-data "$tmp/tdh" --offline
[ "$rc" -eq 1 ] || fail "(h) exited $rc, want 1"
grep -q readsb-sample.jsonl "$out" || fail "(h) output does not mention readsb-sample.jsonl"

echo "PASS: test_fetch_test_captures"
