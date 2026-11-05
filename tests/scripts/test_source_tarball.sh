#!/usr/bin/env bash
# make-source-tarball.sh: contents, reproducibility, that the restricted fixtures are left
# out, and that skycue/ configures from the unpacked tarball alone.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
for f in leaflet-1.9.4.tgz echarts-5.6.0.tgz; do
  [ -f "$here/sources/$f" ] || { echo "SKIP test_source_tarball: sources/$f missing"; exit 0; }
done
v=$(sed -n 's/^project(skycue VERSION \([0-9.]*\).*/\1/p' "$here/skycue/CMakeLists.txt")
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
bash "$here/scripts/make-source-tarball.sh" -o "$tmp/a" >/dev/null 2>&1 || { echo "FAIL: first run"; exit 1; }
bash "$here/scripts/make-source-tarball.sh" -o "$tmp/b" >/dev/null 2>&1 || { echo "FAIL: second run"; exit 1; }
t=$tmp/a/skycue-$v.tar.gz
[ -f "$t" ] || { echo "FAIL: no $t"; exit 1; }
cmp -s "$t" "$tmp/b/skycue-$v.tar.gz" || { echo "FAIL: not reproducible"; exit 1; }
list=$(tar -tzf "$t")
for want in "skycue-$v/skycue/CMakeLists.txt" "skycue-$v/cmake/sources.cmake" "skycue-$v/etc/skycue.conf.example" \
            "skycue-$v/sources/leaflet-1.9.4.tgz" "skycue-$v/sources/echarts-5.6.0.tgz"; do
  grep -qx "$want" <<<"$list" || { echo "FAIL: $want missing"; exit 1; }
done
[ "$(grep -c "^skycue-$v/sources/." <<<"$list")" = 2 ] || { echo "FAIL: other files under sources/"; exit 1; }
grep -q "/www/tiles/\|/www/overlays/" <<<"$list" && { echo "FAIL: map data included"; exit 1; }
grep -q "^skycue-$v/skycue/tests/fixtures/raw\(/\|$\)" <<<"$list" && { echo "FAIL: fixtures/raw included"; exit 1; }
grep -q "^skycue-$v/skycue/tests/fixtures/pingstation/icd-" <<<"$list" && { echo "FAIL: icd-*.json included"; exit 1; }
for want in "skycue-$v/skycue/tests/fixtures/README.md" "skycue-$v/skycue/tests/fixtures/fisb/README.md"; do
  grep -qx "$want" <<<"$list" || { echo "FAIL: $want missing"; exit 1; }
done
# The fuzz seeds that are copies of those fixtures (docs/hardening.md) are left out too;
# the other seeds stay.
seeds=skycue-$v/skycue/fuzz/seeds
for n in $(seq 1 18); do
  grep -qx "$seeds/raw_formats/seed-$n.txt" <<<"$list" && { echo "FAIL: raw_formats/seed-$n.txt included"; exit 1; }
done
for s in pingstation/seed-1.json pingstation/seed-3.json pingstation/seed-4.json web/seed-13.json; do
  grep -qx "$seeds/$s" <<<"$list" && { echo "FAIL: fuzz seed $s included"; exit 1; }
done
for s in raw_formats/seed-19.txt raw_formats/seed-24.bin pingstation/seed-2.json pingstation/seed-5.bin \
         web/seed-12.txt web/seed-14.txt readsb_line/seed-1.jsonl; do
  grep -qx "$seeds/$s" <<<"$list" || { echo "FAIL: fuzz seed $s missing"; exit 1; }
done
# The two web-library tarballs go in as 0644 root:root whatever their mode on disk.
[ "$(tar -tvzf "$t" | grep -c "^-rw-r--r-- 0/0 .* skycue-$v/sources/")" = 2 ] ||
  { echo "FAIL: sources/ members not 0644 0/0"; exit 1; }
tar -xzf "$t" -C "$tmp"

# Once the restricted files are gone from git (history cleanup), the script still
# works: it deletes only what the archive holds. A shallow clone of HEAD with them
# removed and this checkout's script, the library tarballs copied in as 0600.
c=$tmp/clone
git clone -q --depth 1 "file://$here" "$c" 2>/dev/null || { echo "FAIL: cannot clone HEAD"; exit 1; }
git -C "$c" rm -rq skycue/tests/fixtures/raw skycue/tests/fixtures/pingstation/icd-status.json \
  skycue/tests/fixtures/pingstation/icd-status-verbatim.json skycue/tests/fixtures/pingstation/icd-traffic-1090.json \
  skycue/fuzz/seeds/web/seed-13.json || { echo "FAIL: git rm in the clone"; exit 1; }
cp "$here/scripts/make-source-tarball.sh" "$c/scripts/" && git -C "$c" add scripts/make-source-tarball.sh
git -C "$c" -c user.name=test -c user.email=test@invalid commit -qm "drop restricted files" ||
  { echo "FAIL: commit in the clone"; exit 1; }
mkdir -p "$c/sources"
for f in leaflet-1.9.4.tgz echarts-5.6.0.tgz; do cp "$here/sources/$f" "$c/sources/$f"; chmod 0600 "$c/sources/$f"; done
bash "$c/scripts/make-source-tarball.sh" -o "$tmp/c" >"$tmp/c.log" 2>&1 ||
  { cat "$tmp/c.log"; echo "FAIL: make-source-tarball.sh fails once the fixtures are gone from git"; exit 1; }
tc=$tmp/c/skycue-$v.tar.gz
grep -qx "skycue-$v/skycue/CMakeLists.txt" <<<"$(tar -tzf "$tc")" || { echo "FAIL: clone tarball incomplete"; exit 1; }
[ "$(tar -tvzf "$tc" | grep -c "^-rw-r--r-- 0/0 .* skycue-$v/sources/")" = 2 ] ||
  { echo "FAIL: 0600 sources/ files not stored as 0644"; exit 1; }
rm -rf "$c" "$tmp/c"

cmake -S "$tmp/skycue-$v/skycue" -B "$tmp/build" -DSKYCUE_BUILD_TESTS=OFF -DSKYCUE_BUILD_EXAMPLES=OFF >/dev/null 2>&1 ||
  { echo "FAIL: skycue/ does not configure from the tarball"; exit 1; }
echo PASS
