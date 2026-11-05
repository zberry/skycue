#!/usr/bin/env bash
# make-source-tarball.sh: the archive equals the tree (git ls-files plus the two
# pinned web-library tarballs), is reproducible, ignores untracked files, and
# configures from the unpacked tarball alone.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
for f in leaflet-1.9.4.tgz echarts-5.6.0.tgz; do
  [ -f "$here/sources/$f" ] || { echo "SKIP test_source_tarball: sources/$f missing"; exit 77; }
done
git -C "$here" rev-parse --show-toplevel >/dev/null 2>&1 ||
  { echo "SKIP test_source_tarball: not a git checkout (make-source-tarball.sh archives HEAD)"; exit 77; }
v=$(sed -n 's/^project(skycue VERSION \([0-9.]*\).*/\1/p' "$here/CMakeLists.txt")
tmp=$(mktemp -d)
made=()
cleanup() { rm -rf "$tmp"; for f in "${made[@]}"; do rm -f "$f"; done; }
trap cleanup EXIT
bash "$here/scripts/make-source-tarball.sh" -o "$tmp/a" >/dev/null 2>&1 || { echo "FAIL: first run"; exit 1; }
bash "$here/scripts/make-source-tarball.sh" -o "$tmp/b" >/dev/null 2>&1 || { echo "FAIL: second run"; exit 1; }
t=$tmp/a/skycue-$v.tar.gz
[ -f "$t" ] || { echo "FAIL: no $t"; exit 1; }
cmp -s "$t" "$tmp/b/skycue-$v.tar.gz" || { echo "FAIL: not reproducible"; exit 1; }
list=$(tar -tzf "$t")
for want in "skycue-$v/CMakeLists.txt" "skycue-$v/cmake/web-sources.cmake" "skycue-$v/etc/skycue.conf.example" \
            "skycue-$v/sources/leaflet-1.9.4.tgz" "skycue-$v/sources/echarts-5.6.0.tgz"; do
  grep -qx "$want" <<<"$list" || { echo "FAIL: $want missing"; exit 1; }
done
# The archive is the tree: files only, minus the two library tarballs, equal git ls-files.
got=$(grep -v '/$' <<<"$list" | grep -v "^skycue-$v/sources/\(leaflet-1.9.4.tgz\|echarts-5.6.0.tgz\)\$" | LC_ALL=C sort)
want=$(git -C "$here" ls-files | sed "s,^,skycue-$v/," | LC_ALL=C sort)
[ -n "$want" ] && [ "$got" = "$want" ] || {
  echo "FAIL: archive differs from git ls-files:"; diff <(echo "$want") <(echo "$got") | head -n 20; exit 1; }
grep -q "^skycue-$v/skycue/" <<<"$list" && { echo "FAIL: stray skycue/ level"; exit 1; }
# The two web-library tarballs go in as 0644 root:root whatever their mode on disk.
[ "$(tar -tvzf "$t" | grep -c "^-rw-r--r-- 0/0 .* skycue-$v/sources/")" = 2 ] ||
  { echo "FAIL: sources/ members not 0644 0/0"; exit 1; }

# Untracked files (a fetched capture) never reach the archive.
for f in tests/fixtures/raw/x tests/fixtures/fisb/tg30.978; do
  if [ ! -e "$here/$f" ]; then
    mkdir -p "$(dirname "$here/$f")"; : >"$here/$f"; made+=("$here/$f")
  fi
done
bash "$here/scripts/make-source-tarball.sh" -o "$tmp/d" >/dev/null 2>&1 || { echo "FAIL: run with untracked files"; exit 1; }
cmp -s "$t" "$tmp/d/skycue-$v.tar.gz" || { echo "FAIL: untracked files changed the tarball"; exit 1; }

tar -xzf "$t" -C "$tmp"
cmake -S "$tmp/skycue-$v" -B "$tmp/build" -DSKYCUE_BUILD_TESTS=OFF -DSKYCUE_BUILD_EXAMPLES=OFF >/dev/null 2>&1 ||
  { echo "FAIL: the tarball does not configure"; exit 1; }
echo PASS
