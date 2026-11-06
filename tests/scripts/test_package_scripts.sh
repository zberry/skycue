#!/usr/bin/env bash
# build-package.sh / verify-package.sh argument handling and the binfmt check, without podman.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
b=$here/scripts/build-package.sh
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: test_package_scripts: $*"; exit 1; }
# The build container exports its own factor; the checks below set it themselves.
unset SKYCUE_TEST_SLOW_FACTOR
pv=$(sed -n 's/^project(skycue VERSION \([0-9.]*\).*/\1/p' "$here/CMakeLists.txt")
[ -n "$pv" ] || fail "no version in CMakeLists.txt"
bash "$b" nonsense >/dev/null 2>&1; [ $? -eq 2 ] || fail "bad target is not exit 2"
touch "$tmp/skycue-$pv.tar.gz"; mkdir "$tmp/nobinfmt"
arch=$(uname -m); foreign=debian12-arm64; [ "$arch" = aarch64 ] && foreign=debian12-amd64
out=$(SKYCUE_BINFMT_DIR=$tmp/nobinfmt bash "$b" "$foreign" --source "$tmp/skycue-$pv.tar.gz" --dry-run 2>&1); rc=$?
[ $rc -eq 3 ] || fail "missing binfmt: exit $rc, want 3"
grep -q qemu-user-static <<<"$out" || fail "missing binfmt: no qemu-user-static hint"
grep -q 'would run:' <<<"$out" && fail "missing binfmt: printed a dry-run line before stopping"
native=debian12-amd64; [ "$arch" = aarch64 ] && native=debian12-arm64
narch=amd64; [ "$arch" = aarch64 ] && narch=arm64
out=$(bash "$b" "$native" --source "$tmp/skycue-$pv.tar.gz" --dry-run 2>&1) || fail "native dry-run failed: $out"
grep -q 'would run: podman build .*containers/Containerfile.debian12' <<<"$out" || fail "no image build line"
grep -q "would run: podman run .*skycue-$pv.tar.gz" <<<"$out" || fail "no build run line"
# The run: network allowed, the tarball read-only, the artefacts in dist/<target>/, slow factor 1 on the native arch.
run=$(grep 'would run: podman run ' <<<"$out")
grep -q -- '--network none' <<<"$run" && fail "build run is --network none (package builds may use the network)"
grep -q "skycue-$pv.tar.gz:[^ ]*:ro" <<<"$run" || fail "tarball not mounted read-only"
grep -q "dist/${native}:/out:Z" <<<"$run" || fail "artefacts do not go to dist/$native"
grep -q 'SKYCUE_TEST_SLOW_FACTOR=1 ' <<<"$run" || fail "native build is not slow factor 1"
grep -q "would run: podman rmi .*skycue-pkg:$native" <<<"$out" || fail "image not removed by default"
# The Debian release (12 or 13) goes to the inner script, which puts it in the revision.
grep -Eq "would run: podman run .* /build-package.sh deb /src/skycue-$pv.tar.gz $pv debian12$" <<<"$out" ||
  fail "deb build run does not pass the distro: $run"
# A --source tarball named for another release than CMakeLists.txt: usage error, nothing run.
touch "$tmp/skycue-9.9.9.tar.gz"
out=$(bash "$b" "$native" --source "$tmp/skycue-9.9.9.tar.gz" --dry-run 2>&1); rc=$?
[ $rc -eq 2 ] || fail "tarball version mismatch: exit $rc, want 2"
grep -q "9.9.9" <<<"$out" && grep -qF "$pv" <<<"$out" || fail "tarball version mismatch: versions not named: $out"
grep -q 'would run:' <<<"$out" && fail "tarball version mismatch: ran something"
out=$(KEEP_IMAGE=1 bash "$b" "$native" --source "$tmp/skycue-$pv.tar.gz" --dry-run 2>&1) || fail "KEEP_IMAGE dry-run failed"
grep -q 'podman rmi' <<<"$out" && fail "KEEP_IMAGE=1 still removes the image"
# A foreign arch with its (fake) binfmt handler enabled: --arch, slow factor 4 unless the environment says otherwise.
mkdir "$tmp/binfmt"
fh=qemu-aarch64; farch=arm64; [ "$arch" = aarch64 ] && { fh=qemu-x86_64; farch=amd64; }
printf 'enabled\ninterpreter /usr/bin/%s-static\n' "$fh" > "$tmp/binfmt/$fh"
out=$(SKYCUE_BINFMT_DIR=$tmp/binfmt bash "$b" "$foreign" --source "$tmp/skycue-$pv.tar.gz" --dry-run 2>&1) ||
  fail "foreign dry-run with binfmt failed: $out"
grep -q "would run: podman build --arch $farch " <<<"$out" || fail "foreign image build lacks --arch $farch"
grep -q 'would run: podman run .*SKYCUE_TEST_SLOW_FACTOR=4 ' <<<"$out" || fail "foreign build is not slow factor 4"
out=$(SKYCUE_TEST_SLOW_FACTOR=8 SKYCUE_BINFMT_DIR=$tmp/binfmt bash "$b" "$foreign" --source "$tmp/skycue-$pv.tar.gz" --dry-run 2>&1) ||
  fail "foreign dry-run with slow factor 8 failed"
grep -q 'SKYCUE_TEST_SLOW_FACTOR=8 ' <<<"$out" || fail "SKYCUE_TEST_SLOW_FACTOR from the environment ignored"
out=$(SKYCUE_BINFMT_DIR=$tmp/binfmt bash "$b" "$foreign" --source "$tmp/skycue-$pv.tar.gz" --dry-run 2>&1)
grep -q "DEB_BUILD_OPTIONS=parallel=$(nproc) " <<<"$out" || fail "foreign build is not parallel=nproc"
out=$(SKYCUE_BUILD_JOBS=2 SKYCUE_BINFMT_DIR=$tmp/binfmt bash "$b" "$foreign" --source "$tmp/skycue-$pv.tar.gz" --dry-run 2>&1)
grep -q 'DEB_BUILD_OPTIONS=parallel=2 ' <<<"$out" || fail "SKYCUE_BUILD_JOBS ignored"
out=$(bash "$b" "$native" --source "$tmp/skycue-$pv.tar.gz" --dry-run 2>&1)
grep -q 'DEB_BUILD_OPTIONS' <<<"$out" && fail "native build carries DEB_BUILD_OPTIONS"
printf 'disabled\n' > "$tmp/binfmt/$fh"
SKYCUE_BINFMT_DIR=$tmp/binfmt bash "$b" "$foreign" --source "$tmp/skycue-$pv.tar.gz" --dry-run >/dev/null 2>&1
[ $? -eq 3 ] || fail "disabled binfmt handler is not exit 3"
bash "$b" "$native" --source "$tmp/missing.tar.gz" --dry-run >/dev/null 2>&1; [ $? -eq 2 ] || fail "missing --source is not exit 2"
if [ "$arch" = x86_64 ]; then
  out=$(bash "$b" el9 --source "$tmp/skycue-$pv.tar.gz" --dry-run 2>&1) || fail "el9 dry-run failed"
  grep -q 'Containerfile.el9' <<<"$out" || fail "el9 uses the wrong Containerfile"
  grep -q 'would run: podman run .*skycue-pkg:el9 /build-package.sh rpm ' <<<"$out" || fail "el9 does not build an rpm"
fi
# Without --dry-run, against a stand-in podman, in a copy of the repo layout (so nothing lands
# in this checkout's dist/): a failing podman or inner script is exit 1 whatever its status;
# a build that leaves the packages prints their dist/ paths, the deb revision -1~debNNu1.
fr=$tmp/repo
mkdir -p "$fr/scripts" "$fr/containers" "$tmp/pbin"
cp "$b" "$fr/scripts/" && cp "$here/CMakeLists.txt" "$fr/" || fail "cannot copy the repo layout"
cat > "$tmp/pbin/podman" <<'EOS'
#!/bin/bash
# STUB_RUN_RC: the exit status of "podman run"; with 0 the packages named in STUB_PKGS appear in /out.
if [ "$1" = run ]; then
  for a in "$@"; do case $a in *:/out:Z) o=${a%:/out:Z} ;; esac; done
  [ "${STUB_RUN_RC:-0}" = 0 ] || exit "$STUB_RUN_RC"
  for f in $STUB_PKGS; do : > "$o/$f"; done
fi
exit 0
EOS
chmod +x "$tmp/pbin/podman"
for st in 2 3 125; do
  out=$(PATH=$tmp/pbin:$PATH STUB_RUN_RC=$st bash "$fr/scripts/build-package.sh" "$native" --source "$tmp/skycue-$pv.tar.gz" 2>&1); rc=$?
  [ $rc -eq 1 ] || fail "podman run exit $st: build-package exit $rc, want 1"
done
na=${native#*-}
for d in 12 13; do
  out=$(PATH=$tmp/pbin:$PATH STUB_PKGS="skycue_${pv}-1~deb${d}u1_${na}.deb libskycue-dev_${pv}-1~deb${d}u1_${na}.deb" \
        bash "$fr/scripts/build-package.sh" "debian$d-$na" --source "$tmp/skycue-$pv.tar.gz" 2>&1) ||
    fail "stub build debian$d-$na failed: $out"
  [ "$(tail -n 2 <<<"$out")" = "dist/debian$d-$na/skycue_${pv}-1~deb${d}u1_${na}.deb
dist/debian$d-$na/libskycue-dev_${pv}-1~deb${d}u1_${na}.deb" ] || fail "debian$d-$na artefact names: $out"
done
rm -rf "$fr/dist"
out=$(PATH=$tmp/pbin:$PATH STUB_PKGS="skycue_${pv}-1_${na}.deb libskycue-dev_${pv}-1_${na}.deb" \
      bash "$fr/scripts/build-package.sh" "debian12-$na" --source "$tmp/skycue-$pv.tar.gz" 2>&1)
[ $? -eq 1 ] || fail "debs without the ~deb12u1 revision accepted"

# build-package-inner.sh's deb_changelog prepends the release entry to a copy of the changelog:
# <v>-1~deb12u1 for bookworm, <v>-1~deb13u1 for trixie, same identity and date as the entry below.
inner_b=$here/containers/build-package-inner.sh
fns=$(sed -n '/^deb_changelog() {/,/^}/p' "$inner_b")
grep -q '^deb_changelog() {' <<<"$fns" || fail "build-package-inner.sh has no deb_changelog()"
for d in "12 bookworm" "13 trixie"; do
  read -r n code <<<"$d"
  cp "$here/debian/changelog" "$tmp/changelog"
  bash -c "set -euo pipefail; $fns"'
deb_changelog "$1" "$2" "$3"' _ "$tmp/changelog" "$pv" "debian$n" || fail "deb_changelog debian$n failed"
  [ "$(head -n 1 "$tmp/changelog")" = "skycue (${pv}-1~deb${n}u1) $code; urgency=medium" ] ||
    fail "deb_changelog debian$n: first line $(head -n 1 "$tmp/changelog")"
  [ "$(grep -c '^skycue (' "$tmp/changelog")" = "$(( $(grep -c '^skycue (' "$here/debian/changelog") + 1 ))" ] ||
    fail "deb_changelog debian$n: not one entry added"
  [ "$(sed -n '2,$p' "$tmp/changelog" | sed -n '/^skycue (/,$p')" = "$(cat "$here/debian/changelog")" ] ||
    fail "deb_changelog debian$n: the committed entries changed"
  [ "$(grep '^ -- ' "$tmp/changelog" | sort -u | wc -l)" = 1 ] || fail "deb_changelog debian$n: trailer differs from the committed one"
done
# deb_unitdir: /lib/systemd/system on Debian 12 (its debhelper looks only there),
# /usr/lib/systemd/system on 13, printed and written into the copy's skycue.install.
fns_u=$(sed -n '/^deb_unitdir() {/,/^}/p' "$inner_b")
grep -q '^deb_unitdir() {' <<<"$fns_u" || fail "build-package-inner.sh has no deb_unitdir()"
grep -q 'SKYCUE_DEB_UNITDIR=$(deb_unitdir ' "$inner_b" || fail "build-package-inner.sh does not export SKYCUE_DEB_UNITDIR"
for d in "12 /lib/systemd/system" "13 /usr/lib/systemd/system"; do
  read -r n dir <<<"$d"
  rm -rf "$tmp/tree"; mkdir -p "$tmp/tree/debian"; cp "$here/debian/skycue.install" "$tmp/tree/debian/"
  got=$(bash -c "set -euo pipefail; $fns_u"'
deb_unitdir "$1" "$2"' _ "$tmp/tree" "debian$n") || fail "deb_unitdir debian$n failed"
  [ "$got" = "$dir" ] || fail "deb_unitdir debian$n: $got, want $dir"
  grep -qx "${dir#/}/skycued.service" "$tmp/tree/debian/skycue.install" || fail "deb_unitdir debian$n: skycue.install not rewritten"
  [ "$(grep -c 'skycued.service' "$tmp/tree/debian/skycue.install")" = 1 ] || fail "deb_unitdir debian$n: unit listed twice"
  [ "$(grep -vc 'skycued.service' "$tmp/tree/debian/skycue.install")" = "$(grep -vc 'skycued.service' "$here/debian/skycue.install")" ] ||
    fail "deb_unitdir debian$n: other lines changed"
done
cp "$here/debian/changelog" "$tmp/changelog"
bash -c "set -euo pipefail; $fns"'
deb_changelog "$1" "$2" debian11' _ "$tmp/changelog" "$pv" >/dev/null 2>&1 && fail "deb_changelog accepted debian11"

v=$here/scripts/verify-package.sh
bash "$v" nonsense "$tmp/x.deb" >/dev/null 2>&1; [ $? -eq 2 ] || fail "verify: bad target is not exit 2"
bash "$v" $native "$tmp/missing.deb" >/dev/null 2>&1; [ $? -eq 2 ] || fail "verify: missing package is not exit 2"
touch "$tmp/skycue_${pv}-1~deb12u1_${farch}.deb" "$tmp/skycue_${pv}-1~deb12u1_${narch}.deb" "$tmp/libskycue-dev_${pv}-1~deb12u1_${narch}.deb" "$tmp/skycue-${pv}-1.el9.x86_64.rpm"
bash "$v" $native >/dev/null 2>&1; [ $? -eq 2 ] || fail "verify: no package is not exit 2"
bash "$v" $native "$tmp/skycue-${pv}-1.el9.x86_64.rpm" --source "$tmp/skycue-$pv.tar.gz" >/dev/null 2>&1
[ $? -eq 2 ] || fail "verify: an rpm for a deb target is not exit 2"
bash "$v" $native "$tmp/skycue_${pv}-1~deb12u1_${narch}.deb" --source "$tmp/missing.tar.gz" >/dev/null 2>&1
[ $? -eq 2 ] || fail "verify: missing --source is not exit 2"
out=$(SKYCUE_BINFMT_DIR=$tmp/nobinfmt bash "$v" "$foreign" "$tmp/skycue_${pv}-1~deb12u1_${farch}.deb" --source "$tmp/skycue-$pv.tar.gz" --dry-run 2>&1); rc=$?
[ $rc -eq 3 ] || fail "verify: missing binfmt: exit $rc, want 3"
grep -q qemu-user-static <<<"$out" || fail "verify: missing binfmt: no qemu-user-static hint"
# Dry run: phase 1 on the Containerfile's base image (not the build image), phase 2 offline.
out=$(bash "$v" $native "$tmp/skycue_${pv}-1~deb12u1_${narch}.deb" "$tmp/libskycue-dev_${pv}-1~deb12u1_${narch}.deb" \
        --source "$tmp/skycue-$pv.tar.gz" --systemd --dry-run 2>&1) || fail "verify: dry-run failed: $out"
base=$(sed -n 's/^FROM[[:space:]]\{1,\}\([^[:space:]]\{1,\}\).*/\1/p' "$here/containers/Containerfile.debian12" | head -n 1)
[ -n "$base" ] || fail "verify: no FROM in Containerfile.debian12"
p1=$(grep 'would run: podman run ' <<<"$out" | head -n 1)
grep -qF " $base " <<<"$p1" || fail "verify: phase 1 does not start from $base: $p1"
grep -q 'skycue-pkg:' <<<"$out" && fail "verify: uses the build image"
grep -qF "libskycue-dev_${pv}-1~deb12u1_${narch}.deb:/pkgs/libskycue-dev_${pv}-1~deb12u1_${narch}.deb:ro" <<<"$p1" || fail "verify: dev package not mounted read-only"
grep -q ' cmake ' <<<"$p1" || fail "verify: no cmake installed with a dev package"
# Package paths reach the install shell as positional arguments, not inside its script.
grep -q "bash -c '[^']*/pkgs/" <<<"$p1" && fail "verify: package paths inside the bash -c script: $p1"
grep -qF "' _ /pkgs/" <<<"$p1" || grep -qF "' _ '/pkgs/" <<<"$p1" || fail "verify: package paths not passed as arguments: $p1"
grep -q "would run: podman commit .* skycue-verify:${native}\$" <<<"$out" || fail "verify: phase 1 not committed"
p2=$(grep 'would run: podman run --rm ' <<<"$out")
grep -q -- '--network none' <<<"$p2" || fail "verify: phase 2 is not --network none"
grep -q "skycue-$pv.tar.gz:[^ ]*:ro" <<<"$p2" || fail "verify: tarball not mounted read-only in phase 2"
grep -q 'verify-package-inner.sh:[^ ]*:ro' <<<"$p2" || fail "verify: inner script not mounted read-only"
grep -q 'VERIFY_DEV=1' <<<"$p2" || fail "verify: dev package not passed to the checks"
grep -q 'would run: podman run -d .*--systemd=always.*--network none' <<<"$out" || fail "verify: no systemd container"
grep -q "would run: podman rmi -f skycue-verify:$native" <<<"$out" || fail "verify: image not removed"
out=$(bash "$v" $native "$tmp/skycue_${pv}-1~deb12u1_${narch}.deb" --source "$tmp/skycue-$pv.tar.gz" --dry-run 2>&1) ||
  fail "verify: dry-run without dev package failed"
grep -q 'VERIFY_DEV=0' <<<"$out" || fail "verify: VERIFY_DEV not 0 without a dev package"
grep -q 'systemd=always' <<<"$out" && fail "verify: systemd run without --systemd"
if [ "$arch" = x86_64 ]; then
  out=$(bash "$v" el9 "$tmp/skycue-${pv}-1.el9.x86_64.rpm" --source "$tmp/skycue-$pv.tar.gz" --dry-run 2>&1) ||
    fail "verify: el9 dry-run failed: $out"
  grep -q 'would run: podman run .* docker.io/rockylinux/rockylinux:9 .*dnf' <<<"$out" || fail "verify: el9 not on rockylinux:9 with dnf"
fi
# verify-package-inner.sh's unit_fail tells a namespace failure (226/NAMESPACE,
# a rootless podman limit) from a unit fault, with stand-in systemctl/journalctl.
inner=$here/containers/verify-package-inner.sh
mkdir -p "$tmp/sd"
cat > "$tmp/sd/systemctl" <<'EOS'
#!/bin/sh
echo "${STUB_STATUS:-1}"
EOS
cat > "$tmp/sd/journalctl" <<'EOS'
#!/bin/sh
echo "skycued.service: Main process exited, code=exited, status=${STUB_STATUS:-1}/${STUB_NAME:-FAILURE}"
EOS
chmod +x "$tmp/sd/systemctl" "$tmp/sd/journalctl"
fns=$(sed -n '/^fail() /p; /^unit_fail() {/,/^}/p' "$inner")
grep -q '^unit_fail() {' <<<"$fns" || fail "verify-package-inner.sh has no unit_fail()"
for case in "226 NAMESPACE" "1 FAILURE"; do
  read -r st name <<<"$case"
  out=$(PATH=$tmp/sd:$PATH STUB_STATUS=$st STUB_NAME=$name T=el9 bash -c "$fns"'
unit_fail "systemctl start skycued"' 2>&1); rc=$?
  [ $rc -eq 1 ] || fail "unit_fail ($st): exit $rc, want 1"
  line=$(grep '^verify: FAIL' <<<"$out")
  if [ "$st" = 226 ]; then
    grep -q '226/NAMESPACE.*rootless podman limit, not a unit fault' <<<"$line" || fail "unit_fail (226): $line"
  else
    grep -q 'unit fault, exit status 1' <<<"$line" || fail "unit_fail (1): $line"
    grep -q 'NAMESPACE' <<<"$line" && fail "unit_fail (1) blames the namespaces: $line"
  fi
done
# Every unexpected error in verify-package-inner.sh prints a FAIL line (the ERR trap,
# inherited by functions), not just a bare non-zero exit.
fns=$(sed -n '/^set -E/p; /^fail() /p; /^trap .* ERR$/p' "$inner")
[ "$(grep -c . <<<"$fns")" = 3 ] || fail "verify-package-inner.sh lacks set -E, fail() or the ERR trap: $fns"
out=$(T=el9 bash -c "$fns"'
f() { false; }
f
echo not reached' 2>&1); rc=$?
[ $rc -eq 1 ] || fail "ERR trap: exit $rc, want 1"
grep -q '^verify: FAIL el9: unexpected error at line [0-9]*: false$' <<<"$out" || fail "ERR trap: $out"
echo PASS
