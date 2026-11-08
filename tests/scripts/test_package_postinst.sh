#!/usr/bin/env bash
# The upgrade step of the packages: debian/skycue.postinst and the spec's post
# script both carry one block (between "BEGIN convert-conf" and "END
# convert-conf") that turns an old /etc/skycue/skycue.conf into skycue.toml
# with skycued --convert-conf. The block is cut out of each file and run
# here against a fake root (SKYCUE_ROOT) with the built skycued; the real
# package build and install in containers has no upgrade step, so this is
# the upgrade test.
# usage: test_package_postinst.sh CMAKE PACKAGE_CONFIG_CMAKE EXAMPLE SKYCUED OLD_CONF WORK_DIR
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
cmake=${1:-} script=${2:-} example=${3:-} skycued=${4:-} oldconf=${5:-} work=${6:-}
fail() { echo "FAIL: test_package_postinst: $*"; exit 1; }
[ $# = 6 ] || fail "usage: test_package_postinst.sh CMAKE PACKAGE_CONFIG_CMAKE EXAMPLE SKYCUED OLD_CONF WORK_DIR"
for f in "$script" "$example" "$skycued" "$oldconf"; do [ -e "$f" ] || fail "$f missing"; done
rm -rf "$work"; mkdir -p "$work" || fail "cannot create $work"
trap 'rm -rf "$work"' EXIT

"$cmake" -DIN="$example" -DOUT="$work/packaged.toml" -P "$script" >"$work/gen.log" 2>&1 || { cat "$work/gen.log"; fail "PackageConfig.cmake"; }
sed -e 's|^RECEIVER_LAT=.*|RECEIVER_LAT=38.9|' -e 's|^RECEIVER_LON=.*|RECEIVER_LON=-77.0|' \
    -e 's|^STATE_DIR=.*|STATE_DIR=/var/lib/skycue|' "$oldconf" >"$work/old.conf"
printf 'MAX_RANGE_NM=90\n' >"$work/old.overrides.conf"

# new_root DIR: an upgrade in progress: the old conf (mode 0640), the packaged
# skycue.toml the new package just unpacked, and the old overrides file.
new_root() {
  mkdir -p "$1/etc/skycue" "$1/var/lib/skycue"
  cp "$work/old.conf" "$1/etc/skycue/skycue.conf"; chmod 0640 "$1/etc/skycue/skycue.conf"
  cp "$work/packaged.toml" "$1/etc/skycue/skycue.toml"
  cp "$work/old.overrides.conf" "$1/var/lib/skycue/skycue.overrides.conf"
}
# run BLOCK ROOT [SKYCUED]: the block, stdout to $work/out, stderr to $work/err.
run() {
  SKYCUE_ROOT=$2 SKYCUED=${3:-$skycued} sh "$1" >"$work/out" 2>"$work/err"
}

for src in debian/skycue.postinst packaging/rpm/skycue.spec; do
  name=$(basename "$src")
  sed -n '/BEGIN convert-conf/,/END convert-conf/p' "$here/$src" >"$work/block.sh"
  [ "$(grep -c 'skycued --convert-conf\|--convert-conf' "$work/block.sh")" -ge 1 ] || fail "$name: no convert block"
  [ -s "$work/block.sh" ] || fail "$name: block not found"
  sh -n "$work/block.sh" || fail "$name: block does not parse"
  r=$work/$name.root

  # 1. Upgrade: converted once, the old files kept, one line, owner/mode kept.
  new_root "$r"
  run "$work/block.sh" "$r" || fail "$name: block failed"
  [ "$(wc -l <"$work/out")" = 2 ] || { cat "$work/out" "$work/err"; fail "$name: want one line per converted file"; }
  [ ! -s "$work/err" ] || { cat "$work/err"; fail "$name: stderr on a good conversion"; }
  [ -e "$r/etc/skycue/skycue.conf" ] && fail "$name: skycue.conf still there"
  cmp -s "$r/etc/skycue/skycue.conf.converted" "$work/old.conf" || fail "$name: skycue.conf.converted is not the old file"
  cmp -s "$r/etc/skycue/skycue.toml" "$work/packaged.toml" && fail "$name: skycue.toml is still the packaged one"
  [ "$(stat -c %a "$r/etc/skycue/skycue.toml")" = 640 ] || fail "$name: mode of skycue.toml is not the old file's"
  grep -q '^lat = 38.9' "$r/etc/skycue/skycue.toml" || fail "$name: lat not carried over"
  grep -q '^state_dir = "/var/lib/skycue"' "$r/etc/skycue/skycue.toml" || fail "$name: state_dir not carried over"
  "$skycued" --config "$r/etc/skycue/skycue.toml" --dump-env >/dev/null 2>"$work/e" || { cat "$work/e"; fail "$name: skycued rejects the converted file"; }
  [ -e "$r/var/lib/skycue/skycue.overrides.conf" ] && fail "$name: overrides.conf still there"
  [ -e "$r/var/lib/skycue/skycue.overrides.conf.converted" ] || fail "$name: no overrides.conf.converted"
  grep -q 'max_range_nm *= *90' "$r/var/lib/skycue/skycue.overrides.toml" || fail "$name: overrides.toml lacks max_range_nm = 90"

  # 2. Run again: nothing changes, nothing printed.
  find "$r" -type f -exec sha1sum {} + | sort >"$work/before"
  run "$work/block.sh" "$r" || fail "$name: second run failed"
  [ ! -s "$work/out" ] && [ ! -s "$work/err" ] || { cat "$work/out" "$work/err"; fail "$name: second run printed something"; }
  find "$r" -type f -exec sha1sum {} + | sort | cmp -s - "$work/before" || fail "$name: second run changed files"

  # 3. A skycue.toml the operator already edited is not overwritten.
  new_root "$r.edited"
  sed -i 's|^lat = 0.0|lat = 51.5|' "$r.edited/etc/skycue/skycue.toml"
  cp "$r.edited/etc/skycue/skycue.toml" "$work/edited.toml"
  run "$work/block.sh" "$r.edited" || fail "$name: block failed with an edited toml"
  cmp -s "$r.edited/etc/skycue/skycue.toml" "$work/edited.toml" || fail "$name: edited skycue.toml was changed"
  [ -e "$r.edited/etc/skycue/skycue.conf" ] || fail "$name: skycue.conf moved although skycue.toml is the operator's"
  grep -q 'skycue.toml exists' "$work/err" || { cat "$work/err"; fail "$name: no notice about the existing skycue.toml"; }

  # 4. A failing conversion: both files stay, the error is printed, exit 0.
  new_root "$r.bad"
  printf '#!/bin/sh\necho "line 3: nope" >&2\nexit 2\n' >"$work/badskycued"; chmod +x "$work/badskycued"
  run "$work/block.sh" "$r.bad" "$work/badskycued" || fail "$name: a failed conversion failed the block"
  grep -q 'line 3: nope' "$work/err" || { cat "$work/err"; fail "$name: the error is not printed"; }
  cmp -s "$r.bad/etc/skycue/skycue.conf" "$work/old.conf" || fail "$name: old conf changed by a failed conversion"
  cmp -s "$r.bad/etc/skycue/skycue.toml" "$work/packaged.toml" || fail "$name: skycue.toml changed by a failed conversion"
  [ -e "$r.bad/etc/skycue/skycue.conf.converted" ] && fail "$name: .converted after a failed conversion"
  ls "$r.bad/etc/skycue" | grep -q tmp && fail "$name: temporary files left"

  # 5. A fresh install (no old conf): nothing happens.
  mkdir -p "$r.fresh/etc/skycue"; cp "$work/packaged.toml" "$r.fresh/etc/skycue/skycue.toml"
  run "$work/block.sh" "$r.fresh" || fail "$name: block failed on a fresh install"
  [ ! -s "$work/out" ] && [ ! -s "$work/err" ] || fail "$name: output on a fresh install"
  cmp -s "$r.fresh/etc/skycue/skycue.toml" "$work/packaged.toml" || fail "$name: fresh skycue.toml changed"
done

# The two blocks are the same text.
for s in debian/skycue.postinst packaging/rpm/skycue.spec; do
  sed -n '/BEGIN convert-conf/,/END convert-conf/p' "$here/$s" | sed 's/^ *//' >"$work/$(basename "$s").norm"
done
cmp -s "$work/skycue.postinst.norm" "$work/skycue.spec.norm" || fail "the postinst and spec blocks differ"
echo PASS
