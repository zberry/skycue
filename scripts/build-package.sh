#!/usr/bin/env bash
# build-package.sh <target> [--source TARBALL] [--dry-run]
# Builds the skycue packages for one target in a podman container:
#   debian12-amd64 debian12-arm64 debian13-amd64 debian13-arm64  (.deb)
#   el8 el9                                                      (.rpm, x86_64)
# The image skycue-pkg:<target> is built from containers/Containerfile.<distro>
# (that build uses the network for the distro's packages); the package build
# then runs in it from the source tarball (mounted read-only) and may use the
# network, and leaves the packages in dist/<target>/. (Only the checks after the install run without
# network.) Without --source the
# tarball is made first with scripts/make-source-tarball.sh -o dist.
# A target whose architecture differs from this machine's runs under qemu
# user emulation, which needs the qemu binfmt handler; without it the script
# stops (exit 3) and says how to install it. Emulated builds take hours; there
# is no timeout here. SKYCUE_TEST_SLOW_FACTOR (default 4 emulated, else 1)
# scales the test timeouts of the build's ctest run. Emulated Debian builds
# also compile with parallel=4 (qemu user-mode crashes the compiler under the
# host's full core count); SKYCUE_BUILD_JOBS=N overrides that number. Native
# builds keep one job per core.
# A --source tarball named skycue-<version>.tar.gz must be the release in
# CMakeLists.txt. The Debian revision carries the release:
# skycue_<v>-1~deb12u1_<arch>.deb on Debian 12, ~deb13u1 on Debian 13 (the
# inner script adds that changelog entry in its copy of the tree).
# --dry-run prints each podman command as "would run: ..." and runs none.
# The image is removed afterwards unless KEEP_IMAGE=1.
# Exit: 0 built, 1 failure (any podman or inner-script failure), 2 usage,
# 3 missing qemu binfmt support.
set -euo pipefail

usage() {
  echo "usage: $(basename "$0") <debian12-amd64|debian12-arm64|debian13-amd64|debian13-arm64|el8|el9> [--source TARBALL] [--dry-run]" >&2
  exit 2
}

target= source= dry=0
while [ $# -gt 0 ]; do
  case $1 in
    --source) [ $# -ge 2 ] || usage; source=$2; shift ;;
    --dry-run) dry=1 ;;
    -*) usage ;;
    *) [ -z "$target" ] || usage; target=$1 ;;
  esac
  shift
done
case $target in
  debian12-amd64|debian12-arm64|debian13-amd64|debian13-arm64)
    distro=${target%-*} arch=${target#*-} kind=deb ;;
  el8|el9) distro=$target arch=amd64 kind=rpm ;;
  *) usage ;;
esac

case $(uname -m) in
  x86_64) host_arch=amd64 ;;
  aarch64) host_arch=arm64 ;;
  *) host_arch=$(uname -m) ;;
esac

# Foreign architecture: the qemu binfmt handler must be registered and
# enabled before anything else happens.
if [ "$arch" != "$host_arch" ]; then
  case $arch in arm64) handler=qemu-aarch64 ;; amd64) handler=qemu-x86_64 ;; esac
  binfmt=${SKYCUE_BINFMT_DIR:-/proc/sys/fs/binfmt_misc}
  if [ "$(head -n 1 "$binfmt/$handler" 2>/dev/null)" != enabled ]; then
    cat >&2 <<EOF
build-package: $target is an $arch target and this machine is $host_arch: podman runs it
under qemu user emulation, but no enabled $handler handler is registered in $binfmt.
Install qemu-user-static, which registers the handlers through binfmt_misc:
  Debian/Ubuntu:  sudo apt-get install qemu-user-static
  Fedora/EL:      sudo dnf install qemu-user-static
then check that $binfmt/$handler exists and its first line is "enabled"
(sudo systemctl restart systemd-binfmt, or reboot, if it does not), and run this again.
EOF
    exit 3
  fi
fi

repo=$(dirname "$(dirname "$(readlink -f "$0")")")
v=$(sed -n 's/^project(skycue VERSION \([0-9.]*\).*/\1/p' "$repo/CMakeLists.txt")
[ -n "$v" ] || { echo "build-package: no project(skycue VERSION ...) in CMakeLists.txt" >&2; exit 1; }

# run CMD...: runs CMD, or with --dry-run prints it.
run() {
  if [ "$dry" = 1 ]; then
    local a q line=
    for a in "$@"; do
      if [[ $a =~ ^[A-Za-z0-9_./:=,@%+-]+$ ]]; then q=$a; else printf -v q '%q' "$a"; fi
      line+=" $q"
    done
    echo "would run:$line"
  else
    "$@"
  fi
}

if [ -n "$source" ]; then
  [ -f "$source" ] || { echo "build-package: no such source tarball: $source" >&2; exit 2; }
  tarball=$(readlink -f "$source")
  # The release the tarball's name gives (as verify-package.sh reads it) must be this tree's.
  case $(basename "$tarball") in
    skycue-*.tar.gz) tv=$(basename "$tarball" .tar.gz); tv=${tv#skycue-}
      if [[ $tv =~ ^[0-9][0-9.]*$ ]] && [ "$tv" != "$v" ]; then
        echo "build-package: $source is release $tv but CMakeLists.txt is $v" >&2
        exit 2
      fi ;;
  esac
else
  tarball=$repo/dist/skycue-$v.tar.gz
  if [ "$dry" = 1 ]; then
    echo "would run: (cd $repo && scripts/make-source-tarball.sh -o dist)"
  else
    (cd "$repo" && bash scripts/make-source-tarball.sh -o dist) ||
      { echo "build-package: make-source-tarball.sh failed" >&2; exit 1; }
    [ -f "$tarball" ] || { echo "build-package: make-source-tarball.sh wrote no $tarball" >&2; exit 1; }
  fi
fi

slow=${SKYCUE_TEST_SLOW_FACTOR:-}
if [ -z "$slow" ]; then
  if [ "$arch" != "$host_arch" ]; then slow=4; else slow=1; fi
fi

jobs_opt=()
if [ "$arch" != "$host_arch" ]; then
  jobs_opt=(-e "DEB_BUILD_OPTIONS=parallel=${SKYCUE_BUILD_JOBS:-4}")
fi

image=skycue-pkg:$target
out=$repo/dist/$target
cleanup() {
  if [ "${KEEP_IMAGE:-0}" != 1 ]; then
    run podman rmi -f "$image" >/dev/null 2>&1 || true
  fi
}
if [ "$dry" != 1 ]; then
  trap cleanup EXIT
  mkdir -p "$out"
fi

# Any failure of podman or of the inner script (whatever its status, 2 for
# its own usage errors included) is a build failure here: exit 1.
run podman build --arch "$arch" -f "$repo/containers/Containerfile.$distro" -t "$image" "$repo/containers" || {
  echo "build-package: $target: podman build failed (exit $?)" >&2; exit 1; }
inner=("$kind" "/src/skycue-$v.tar.gz" "$v")
[ "$kind" = rpm ] || inner+=("$distro")
# :Z relabels the mounts for an SELinux-enforcing host (a no-op elsewhere).
run podman run --rm --arch "$arch" \
  -e "SKYCUE_TEST_SLOW_FACTOR=$slow" \
  ${jobs_opt[@]+"${jobs_opt[@]}"} \
  -v "$tarball:/src/skycue-$v.tar.gz:ro,Z" \
  -v "$out:/out:Z" \
  "$image" /build-package.sh "${inner[@]}" || {
  echo "build-package: $target: the package build failed (exit $?)" >&2; exit 1; }

if [ "$dry" = 1 ]; then
  [ "${KEEP_IMAGE:-0}" = 1 ] || run podman rmi -f "$image"
  exit 0
fi

# The release artefacts (Debian -dbgsym packages may sit beside them).
case $kind in
  deb) rev=1~deb${distro#debian}u1
       want=("skycue_${v}-${rev}_${arch}.deb" "libskycue-dev_${v}-${rev}_${arch}.deb") ;;
  rpm) want=("skycue-${v}-1.${distro}.x86_64.rpm" "skycue-devel-${v}-1.${distro}.x86_64.rpm") ;;
esac
for f in "${want[@]}"; do
  [ -f "$out/$f" ] || { ls -l "$out" >&2; echo "build-package: $target: no $f in dist/$target" >&2; exit 1; }
done
for f in "${want[@]}"; do echo "dist/$target/$f"; done
