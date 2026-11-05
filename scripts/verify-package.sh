#!/usr/bin/env bash
# verify-package.sh <target> <package...> [--source TARBALL] [--systemd] [--dry-run]
# Clean-room check of built skycue packages for one target (the targets of
# build-package.sh) with podman.
# Phase 1 (network allowed): a fresh container of the target's base image
# (the FROM line of containers/Containerfile.<distro>, not the build image)
# installs the given packages with apt-get or dnf, plus curl, python3 and
# systemd (and cmake and a C++ compiler when a dev package is given), and is
# committed as skycue-verify:<target>.
# Phase 2: that image runs containers/verify-package-inner.sh with
# --network none, the source tarball (default dist/skycue-<version>.tar.gz)
# and the script mounted read-only: version, static unit check, user and
# directories, the shipped config's error, the daemon run as the skycue user
# with a replay and the web checks, skycue-make-maps --dry-run,
# --check-inputs against a closed port, and with a dev package the parent
# project built against it.
# --systemd: then the same image is started with systemd as PID 1 (still no
# network) and the unit is started, checked and made to fail on a missing
# receiver position (verify-package-inner.sh systemd). A unit that fails
# with exit 226/NAMESPACE is reported as such: systemd could not set up the
# sandbox in this container (a rootless podman limit), which is not a fault of
# the unit.
# Prints "verify: OK <target>" as its last line; a failure prints
# "verify: FAIL <target>: <check>" and exits 1. --dry-run prints each podman
# command as "would run: ..." and runs none. The image is removed afterwards
# unless KEEP_IMAGE=1.
# Exit: 0 verified, 1 a check failed, 2 usage (bad target, missing package or
# tarball), 3 missing qemu binfmt support for a foreign-arch target.
set -euo pipefail

usage() {
  echo "usage: $(basename "$0") <debian12-amd64|debian12-arm64|debian13-amd64|debian13-arm64|el8|el9> <package...> [--source TARBALL] [--systemd] [--dry-run]" >&2
  exit 2
}

target= source= systemd=0 dry=0
pkgs=()
while [ $# -gt 0 ]; do
  case $1 in
    --source) [ $# -ge 2 ] || usage; source=$2; shift ;;
    --systemd) systemd=1 ;;
    --dry-run) dry=1 ;;
    -*) usage ;;
    *) if [ -z "$target" ]; then target=$1; else pkgs+=("$1"); fi ;;
  esac
  shift
done
case $target in
  debian12-amd64|debian12-arm64|debian13-amd64|debian13-arm64)
    distro=${target%-*} arch=${target#*-} kind=deb ;;
  el8|el9) distro=$target arch=amd64 kind=rpm ;;
  *) usage ;;
esac
[ ${#pkgs[@]} -gt 0 ] || usage

repo=$(dirname "$(dirname "$(readlink -f "$0")")")
dev=0
abs=()
for p in "${pkgs[@]}"; do
  [ -f "$p" ] || { echo "verify-package: no such package: $p" >&2; exit 2; }
  case $p in
    *.$kind) ;;
    *) echo "verify-package: $p is not a .$kind (target $target)" >&2; exit 2 ;;
  esac
  case $(basename "$p") in libskycue-dev_*.deb|skycue-devel-*.rpm) dev=1 ;; esac
  abs+=("$(readlink -f "$p")")
done

v=$(sed -n 's/^project(skycue VERSION \([0-9.]*\).*/\1/p' "$repo/skycue/CMakeLists.txt")
[ -n "$v" ] || { echo "verify-package: no project(skycue VERSION ...) in skycue/CMakeLists.txt" >&2; exit 1; }
source=${source:-$repo/dist/skycue-$v.tar.gz}
[ -f "$source" ] || {
  echo "verify-package: no source tarball $source (make it with scripts/make-source-tarball.sh -o dist, or pass --source)" >&2
  exit 2
}
tarball=$(readlink -f "$source")
# The release the packages claim is the tarball's when its name gives one.
case $(basename "$tarball") in
  skycue-*.tar.gz) tv=$(basename "$tarball" .tar.gz); tv=${tv#skycue-}
                   [[ $tv =~ ^[0-9][0-9.]*$ ]] && v=$tv ;;
esac

case $(uname -m) in
  x86_64) host_arch=amd64 ;;
  aarch64) host_arch=arm64 ;;
  *) host_arch=$(uname -m) ;;
esac
if [ "$arch" != "$host_arch" ]; then
  case $arch in arm64) handler=qemu-aarch64 ;; amd64) handler=qemu-x86_64 ;; esac
  binfmt=${SKYCUE_BINFMT_DIR:-/proc/sys/fs/binfmt_misc}
  if [ "$(head -n 1 "$binfmt/$handler" 2>/dev/null)" != enabled ]; then
    cat >&2 <<EOF
verify-package: $target is an $arch target and this machine is $host_arch: podman runs it
under qemu user emulation, but no enabled $handler handler is registered in $binfmt.
Install qemu-user-static (Debian/Ubuntu: sudo apt-get install qemu-user-static;
Fedora/EL: sudo dnf install qemu-user-static), check that $binfmt/$handler
says "enabled", and run this again.
EOF
    exit 3
  fi
fi

containerfile=$repo/containers/Containerfile.$distro
base=$(sed -n 's/^FROM[[:space:]]\{1,\}\([^[:space:]]\{1,\}\).*/\1/p' "$containerfile" | head -n 1)
[ -n "$base" ] || { echo "verify-package: no FROM line in $containerfile" >&2; exit 1; }

# run CMD...: runs CMD, or with --dry-run prints it (shell-quoted).
run() {
  if [ "$dry" = 1 ]; then
    local a line=
    for a in "$@"; do
      if [[ $a =~ ^[A-Za-z0-9_./:=,@%+-]+$ ]]; then line+=" $a"; else line+=" '${a//\'/\'\\\'\'}'"; fi
    done
    echo "would run:$line"
  else
    "$@"
  fi
}
fail() { echo "verify: FAIL $target: $*" >&2; exit 1; }

image=skycue-verify:$target
ctr=skycue-verify-$target-$$
sdctr=skycue-verify-sd-$target-$$
cleanup() {
  podman rm -f "$ctr" "$sdctr" >/dev/null 2>&1 || true
  if [ "${KEEP_IMAGE:-0}" != 1 ]; then
    podman rmi -f "$image" >/dev/null 2>&1 || true
  fi
}
[ "$dry" = 1 ] || trap cleanup EXIT

# Phase 1: install, with the network.
mounts=() inpkgs=()
for p in "${abs[@]}"; do
  mounts+=(-v "$p:/pkgs/$(basename "$p"):ro,Z")
  inpkgs+=("/pkgs/$(basename "$p")")
done
case $kind in
  deb)
    tools=(curl python3 systemd)
    [ "$dev" = 0 ] || tools+=(cmake g++ make)
    install='set -e; export DEBIAN_FRONTEND=noninteractive; apt-get update; apt-get install -y --no-install-recommends "$@"; rm -rf /var/lib/apt/lists/*'
    ;;
  rpm)
    # /usr/bin/curl: EL9's base image has curl-minimal, which conflicts with
    # the curl package; the file dependency accepts either.
    tools=(/usr/bin/curl python3 systemd tar gzip)
    [ "$dev" = 0 ] || tools+=(cmake gcc-c++ make)
    install='set -e; dnf -y install "$@"; dnf clean all'
    ;;
esac
# The package paths and tools are the script's arguments, never part of its text.
run podman run --name "$ctr" --arch "$arch" "${mounts[@]}" "$base" bash -c "$install" _ "${inpkgs[@]}" "${tools[@]}" ||
  fail "phase 1: installing the packages on $base"
run podman commit "$ctr" "$image" || fail "phase 1: podman commit"
run podman rm "$ctr" || true

# Phase 2: the checks, offline. verify-package-inner.sh prints its own FAIL
# line and exits 1; any other status is podman's.
env=(-e "VERIFY_TARGET=$target" -e "VERIFY_VERSION=$v" -e "VERIFY_KIND=$kind" -e "VERIFY_DEV=$dev")
ro=(-v "$tarball:/src.tar.gz:ro,Z" -v "$repo/containers/verify-package-inner.sh:/verify-package-inner.sh:ro,Z")
rc=0
run podman run --rm --network none --arch "$arch" "${env[@]}" "${ro[@]}" "$image" bash /verify-package-inner.sh || rc=$?
[ "$rc" = 0 ] || { [ "$rc" = 1 ] && exit 1; fail "phase 2: podman run exited $rc"; }

# --systemd: the unit under systemd as PID 1, still offline. The init path
# is systemd's own binary, present on every base (merged /usr): Debian's
# images have no /sbin/init without systemd-sysv.
if [ "$systemd" = 1 ]; then
  run podman run -d --name "$sdctr" --systemd=always --network none --arch "$arch" "${env[@]}" "${ro[@]}" \
    "$image" /usr/lib/systemd/systemd || fail "systemd: starting the container"
  rc=0
  run podman exec "$sdctr" bash /verify-package-inner.sh systemd || rc=$?
  [ "$rc" = 0 ] || {
    [ "$dry" = 1 ] || podman logs "$sdctr" 2>&1 | tail -n 20 >&2 || true
    [ "$rc" = 1 ] && exit 1; fail "systemd: podman exec exited $rc"
  }
  run podman rm -f "$sdctr" || true
fi

if [ "$dry" = 1 ]; then
  [ "${KEEP_IMAGE:-0}" = 1 ] || run podman rmi -f "$image"
  echo "verify: dry run for $target, nothing was run"
  exit 0
fi
echo "verify: OK $target"
