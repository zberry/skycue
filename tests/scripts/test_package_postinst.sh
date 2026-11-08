#!/usr/bin/env bash
# The upgrade step of the packages: debian/skycue.postinst and the spec's post
# script carry the same shared functions (BEGIN/END convert-conf-shared) and
# a wrapper each (convert-conf-deb, convert-conf-rpm) that turn an old
# /etc/skycue/skycue.conf into skycue.toml with skycued --convert-conf. The
# blocks are cut out and run here against a fake root (SKYCUE_ROOT) with the
# built skycued, and so is debian/skycue.postrm; the container flow has no
# upgrade step, so this is the upgrade test. The shell is dash when installed,
# else sh (then the POSIX-only claim is not checked by a strict shell).
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

sh_cmd=sh
if command -v dash >/dev/null 2>&1; then sh_cmd=dash; else echo "note: dash not installed, using sh ($(readlink -f "$(command -v sh)"))"; fi

# pristine: the unedited packaged file the package ships in share/skycue.
# new_root DIR [conf] [toml]: a fake root. conf: yes|no (old skycue.conf, mode
# 0640); toml: none|pristine|edited; always the old overrides file and the
# state directory.
new_root() {
  local r=$1 conf=${2:-yes} toml=${3:-pristine}
  mkdir -p "$r/etc/skycue" "$r/var/lib/skycue" "$r/usr/share/skycue"
  cp "$work/packaged.toml" "$r/usr/share/skycue/skycue.toml.example"
  [ "$conf" = yes ] && { cp "$work/old.conf" "$r/etc/skycue/skycue.conf"; chmod 0640 "$r/etc/skycue/skycue.conf"; }
  case $toml in
    pristine) cp "$work/packaged.toml" "$r/etc/skycue/skycue.toml" ;;
    edited) sed 's|^port = 8080|port = 8081|' "$work/packaged.toml" >"$r/etc/skycue/skycue.toml" ;;
  esac
  cp "$work/old.overrides.conf" "$r/var/lib/skycue/skycue.overrides.conf"
}
# run SCRIPT ROOT [SKYCUED]
run() { SKYCUE_ROOT=$2 SKYCUED=${3:-$skycued} $sh_cmd "$1" >"$work/out" 2>"$work/err"; }
snap() { find "$1" -type f -exec sha1sum {} + | sort; }

shared() { sed -n '/BEGIN convert-conf-shared/,/END convert-conf-shared/p' "$here/$1"; }
check_converted() {   # NAME ROOT
  local n=$1 r=$2
  [ -e "$r/etc/skycue/skycue.conf" ] && fail "$n: skycue.conf still there"
  cmp -s "$r/etc/skycue/skycue.conf.converted" "$work/old.conf" || fail "$n: skycue.conf.converted is not the old file"
  cmp -s "$r/etc/skycue/skycue.toml" "$work/packaged.toml" && fail "$n: skycue.toml is still the packaged one"
  [ "$(stat -c %a "$r/etc/skycue/skycue.toml")" = 640 ] || fail "$n: mode of skycue.toml is not the old file's"
  grep -q '^lat = 38.9' "$r/etc/skycue/skycue.toml" || fail "$n: lat not carried over"
  grep -q '^state_dir = "/var/lib/skycue"' "$r/etc/skycue/skycue.toml" || fail "$n: state_dir not carried over"
  "$skycued" --config "$r/etc/skycue/skycue.toml" --dump-env >/dev/null 2>"$work/e" || { cat "$work/e"; fail "$n: skycued rejects the converted file"; }
  [ -e "$r/var/lib/skycue/skycue.overrides.conf" ] && fail "$n: overrides.conf still there"
  grep -q 'max_range_nm *= *90' "$r/var/lib/skycue/skycue.overrides.toml" || fail "$n: overrides.toml lacks max_range_nm = 90"
  [ "$(wc -l <"$work/out")" = 2 ] || { cat "$work/out" "$work/err"; fail "$n: want one line per converted file"; }
  # The converter's notice (the comment lines not carried over) is shown, nothing else.
  grep -q 'comment lines of .*skycue.conf not carried over' "$work/err" || { cat "$work/err"; fail "$n: the dropped-comments notice is missing"; }
  [ "$(wc -l <"$work/err")" = 1 ] || { cat "$work/err"; fail "$n: more than the notice on stderr"; }
}
check_untouched_conf() {  # NAME ROOT: the conf stays, the toml is as it was
  local n=$1 r=$2
  cmp -s "$r/etc/skycue/skycue.conf" "$work/old.conf" || fail "$n: old conf changed"
  [ -e "$r/etc/skycue/skycue.conf.converted" ] && fail "$n: .converted without a conversion"
  ls "$r/etc/skycue" "$r/var/lib/skycue" | grep -q 'tmp' && fail "$n: temporary files left"
  return 0
}

for fmt in deb rpm; do
  if [ $fmt = deb ]; then src=debian/skycue.postinst; else src=packaging/rpm/skycue.spec; fi
  { shared "$src"; sed -n "/BEGIN convert-conf-$fmt/,/END convert-conf-$fmt/p" "$here/$src"; } >"$work/$fmt.sh"
  [ "$(grep -c 'BEGIN convert-conf' "$work/$fmt.sh")" = 2 ] || fail "$fmt: blocks not found in $src"
  $sh_cmd -n "$work/$fmt.sh" || fail "$fmt: block does not parse"
  n=$fmt r=$work/$fmt.root
  gen=0; [ $fmt = deb ] && gen=1

  # A. Fresh install, no old conf. deb creates skycue.toml from the shipped
  # copy; rpm has installed it itself.
  new_root "$r.a" no $([ $fmt = deb ] && echo none || echo pristine)
  run "$work/$fmt.sh" "$r.a" || fail "$n A: block failed"
  cmp -s "$r.a/etc/skycue/skycue.toml" "$work/packaged.toml" || fail "$n A: skycue.toml is not the packaged one"
  [ ! -s "$work/err" ] || { cat "$work/err"; fail "$n A: stderr"; }
  # The overrides file converts in any case.
  [ -e "$r.a/var/lib/skycue/skycue.overrides.toml" ] || fail "$n A: overrides not converted"

  # B. Fresh install with an old conf left behind: converted.
  new_root "$r.b" yes $([ $fmt = deb ] && echo none || echo pristine)
  run "$work/$fmt.sh" "$r.b" || fail "$n B: block failed"
  check_converted "$n B" "$r.b"

  # C. Upgrade, unedited skycue.toml and an old conf: converted; again: no change.
  new_root "$r.c" yes pristine
  run "$work/$fmt.sh" "$r.c" || fail "$n C: block failed"
  check_converted "$n C" "$r.c"
  snap "$r.c" >"$work/before"
  run "$work/$fmt.sh" "$r.c" || fail "$n C: second run failed"
  [ ! -s "$work/out" ] && [ ! -s "$work/err" ] || { cat "$work/out" "$work/err"; fail "$n C: second run printed something"; }
  snap "$r.c" | cmp -s - "$work/before" || fail "$n C: second run changed files"

  # D. Upgrade, skycue.toml edited: untouched, notice, the conf stays.
  new_root "$r.d" yes edited
  cp "$r.d/etc/skycue/skycue.toml" "$work/edited.toml"
  run "$work/$fmt.sh" "$r.d" || fail "$n D: block failed"
  cmp -s "$r.d/etc/skycue/skycue.toml" "$work/edited.toml" || fail "$n D: edited skycue.toml was changed"
  check_untouched_conf "$n D" "$r.d"
  grep -q 'skycue.toml exists' "$work/err" || { cat "$work/err"; fail "$n D: no notice"; }
  snap "$r.d" >"$work/before"
  run "$work/$fmt.sh" "$r.d" || fail "$n D: second run failed"
  snap "$r.d" | cmp -s - "$work/before" || fail "$n D: second run changed files"

  # E. skycued missing (127) or failing: the block returns 0, files untouched, reason printed.
  for bad in "$work/no-such-skycued" "$work/badskycued"; do
    printf '#!/bin/sh\necho "line 3: nope" >&2\nexit 2\n' >"$work/badskycued"; chmod +x "$work/badskycued"
    new_root "$r.e" yes pristine
    snap "$r.e" >"$work/before"
    run "$work/$fmt.sh" "$r.e" "$bad" || fail "$n E: a failed conversion failed the block ($bad)"
    grep -q 'could not convert' "$work/err" || { cat "$work/err"; fail "$n E: the failure is not printed ($bad)"; }
    [ ! -e "$bad" ] || grep -q 'line 3: nope' "$work/err" || fail "$n E: skycued's message not shown"
    check_untouched_conf "$n E" "$r.e"
    # deb: a failed conversion on a fresh install still leaves a usable skycue.toml.
    snap "$r.e" | cmp -s - "$work/before" || fail "$n E: files changed by a failed conversion"
    rm -rf "$r.e"
  done
  if [ $fmt = deb ]; then
    new_root "$r.e2" yes none
    run "$work/deb.sh" "$r.e2" "$work/no-such-skycued" || fail "deb E2: block failed"
    cmp -s "$r.e2/etc/skycue/skycue.toml" "$work/packaged.toml" || fail "deb E2: no packaged skycue.toml after a failed conversion"
    [ -e "$r.e2/etc/skycue/skycue.conf" ] || fail "deb E2: skycue.conf moved"
  fi
done

# The shared parts of the two scripts are the same text.
for s in debian/skycue.postinst packaging/rpm/skycue.spec; do
  shared "$s" | sed 's/^ *//' >"$work/$(basename "$s").norm"
done
[ -s "$work/skycue.postinst.norm" ] && cmp -s "$work/skycue.postinst.norm" "$work/skycue.spec.norm" || fail "the shared blocks of postinst and spec differ"

# postrm purge: an edited skycue.toml stays, the one postinst created goes.
pm=$here/debian/skycue.postrm
new_root "$work/pg1" no none
run "$work/deb.sh" "$work/pg1" || fail "purge: postinst block"
[ -s "$work/pg1/var/lib/skycue/skycue.toml.generated-sha256" ] || fail "purge: no checksum recorded"
rm -rf "$work/pg1/usr/share/skycue"        # the package files are gone at purge
SKYCUE_ROOT=$work/pg1 $sh_cmd "$pm" purge || fail "postrm purge failed"
[ -e "$work/pg1/etc/skycue/skycue.toml" ] && fail "purge: unedited skycue.toml kept"
[ -e "$work/pg1/var/lib/skycue/skycue.toml.generated-sha256" ] && fail "purge: checksum file kept"
new_root "$work/pg2" no none
run "$work/deb.sh" "$work/pg2" || fail "purge: postinst block"
echo "# mine" >>"$work/pg2/etc/skycue/skycue.toml"
SKYCUE_ROOT=$work/pg2 $sh_cmd "$pm" purge || fail "postrm purge failed (edited)"
grep -q '^# mine' "$work/pg2/etc/skycue/skycue.toml" || fail "purge: an edited skycue.toml was removed"
SKYCUE_ROOT=$work/pg2 $sh_cmd "$pm" remove || fail "postrm remove failed"
echo PASS
