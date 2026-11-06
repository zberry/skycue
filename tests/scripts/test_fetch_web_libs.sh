#!/usr/bin/env bash
# fetch-web-libs.sh: reads the declarations, downloads (here over file:// URLs
# from a declarations file written by this test, so no network), checks the
# SHA-256, keeps good files, deletes bad ones, and --offline never downloads.
# Also checks that the real cmake/web-sources.cmake parses to two entries.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
script="$here/scripts/dev/fetch-web-libs.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
out="$tmp/out"
fail() { echo "FAIL: test_fetch_web_libs: $*"; [ -f "$out" ] && cat "$out"; exit 1; }
command -v curl >/dev/null || { echo "SKIP test_fetch_web_libs: no curl"; exit 77; }
[ -f "$script" ] || fail "scripts/dev/fetch-web-libs.sh missing"

sha() { sha256sum "$1" | cut -d' ' -f1; }

# --- the real declarations: offline into an empty directory names both files ---
bash "$script" --offline --dest "$tmp/real" >"$out" 2>&1 && fail "--offline into an empty dir succeeded"
for f in leaflet-1.9.4.tgz echarts-5.6.0.tgz; do
  grep -q "missing: $f: download https://" "$out" || fail "--offline did not name $f"
done
# each name, URL and hash in the report comes from cmake/web-sources.cmake
for f in $(sed -n 's/.* SHA256 \([0-9a-f]\{64\}\) .*/\1/p' "$here/cmake/web-sources.cmake"); do
  grep -q "$f" "$out" || fail "hash $f from cmake/web-sources.cmake not reported"
done

# --- synthetic upstream and declarations ---
up=$tmp/up
mkdir -p "$up"
printf 'alpha\n' >"$up/alpha-1.0.tgz"
printf 'beta\n' >"$up/beta-2.0.tgz"
decl=$tmp/decl.cmake
cat >"$decl" <<EOF
# comment line
skycue_declare_source(alpha URL "file://$up/alpha-1.0.tgz" SHA256 $(sha "$up/alpha-1.0.tgz") FILE "alpha-1.0.tgz")
skycue_declare_source(beta URL "file://$up/beta-2.0.tgz" SHA256 $(sha "$up/beta-2.0.tgz") FILE "beta-2.0.tgz")
EOF

# 1. download both
d=$tmp/d1
bash "$script" --dest "$d" --declarations "$decl" >"$out" 2>&1 || fail "download failed"
cmp -s "$d/alpha-1.0.tgz" "$up/alpha-1.0.tgz" || fail "alpha not downloaded"
cmp -s "$d/beta-2.0.tgz" "$up/beta-2.0.tgz" || fail "beta not downloaded"
[ -z "$(ls "$d" | grep '\.part$')" ] || fail "a .part file was left behind"

# 2. second run: nothing downloaded (upstream removed), files kept
mv "$up" "$tmp/up-gone"
bash "$script" --dest "$d" --declarations "$decl" >"$out" 2>&1 || fail "second run failed"
grep -q downloading "$out" && fail "second run downloaded again"
mv "$tmp/up-gone" "$up"

# 3. --offline with one file present: succeeds only when both are there
d=$tmp/d3
mkdir -p "$d"
cp "$up/alpha-1.0.tgz" "$d/"
bash "$script" --offline --dest "$d" --declarations "$decl" >"$out" 2>&1 && fail "--offline succeeded with beta missing"
grep -q "missing: beta-2.0.tgz" "$out" || fail "--offline did not name beta"
[ -e "$d/beta-2.0.tgz" ] && fail "--offline created beta"
cp "$up/beta-2.0.tgz" "$d/"
bash "$script" --offline --dest "$d" --declarations "$decl" >"$out" 2>&1 || fail "--offline failed with both present"

# 4. a file with the wrong hash is deleted and the run fails
d=$tmp/d4
mkdir -p "$d"
printf 'corrupt\n' >"$d/alpha-1.0.tgz"
bash "$script" --dest "$d" --declarations "$decl" >"$out" 2>&1 && fail "accepted a corrupt file"
[ -e "$d/alpha-1.0.tgz" ] && fail "corrupt file not deleted"
grep -q "did not match its pinned SHA-256" "$out" || fail "no hash message"

# 5. a download whose content does not match is refused and not kept
d=$tmp/d5
decl2=$tmp/decl2.cmake
printf 'skycue_declare_source(alpha URL "file://%s/alpha-1.0.tgz" SHA256 %s FILE "alpha-1.0.tgz")\n' \
  "$up" "$(printf '%064d' 0)" >"$decl2"
bash "$script" --dest "$d" --declarations "$decl2" >"$out" 2>&1 && fail "accepted a download with the wrong hash"
[ -e "$d/alpha-1.0.tgz" ] && fail "wrong-hash download kept"
[ -e "$d/alpha-1.0.tgz.part" ] && fail "wrong-hash .part kept"

# 6. an unreachable URL fails cleanly
d=$tmp/d6
printf 'skycue_declare_source(alpha URL "file://%s/nope.tgz" SHA256 %s FILE "alpha-1.0.tgz")\n' \
  "$up" "$(sha "$up/alpha-1.0.tgz")" >"$tmp/decl3.cmake"
bash "$script" --dest "$d" --declarations "$tmp/decl3.cmake" >"$out" 2>&1 && fail "succeeded on a missing URL"
grep -q "download of .* failed" "$out" || fail "no download-failure message"

# 7. usage errors exit 2; a declarations file with no entries exits 1
bash "$script" --bogus >"$out" 2>&1
[ $? -eq 2 ] || fail "bad option did not exit 2"
printf '# nothing\n' >"$tmp/empty.cmake"
bash "$script" --dest "$tmp/d7" --declarations "$tmp/empty.cmake" >"$out" 2>&1 && fail "empty declarations accepted"

echo "PASS test_fetch_web_libs"
