#!/usr/bin/env bash
# build-package-inner.sh <deb|rpm> <tarball> <version> [<debian12|debian13>]:
# runs inside a skycue-pkg:<target> image as /build-package.sh
# (packaging/build-package.sh starts it; the network is available). Builds the
# binary packages from the source tarball and copies them to /out.
#   deb: unpack, prepend a <version>-1~debNNu1 entry to the unpacked tree's
#        debian/changelog (the release in the revision; the committed
#        changelog stays <version>-1) and put the unit where that release's
#        debhelper finds it (deb_unitdir), dpkg-buildpackage -b -us -uc in the
#        tree (it checks the Build-Depends of debian/control), copy ../*.deb
#        to /out. The distro argument is required for deb.
#   rpm: rpmbuild -bb with the tarball as the only source and the spec taken
#        from the tarball, copy the rpms to /out.
# Both run the package's tests (dh_auto_test, %check); SKYCUE_TEST_SLOW_FACTOR
# scales their timeouts (debian/rules reads it from the environment, the spec
# from the skycue_slow_factor define).
set -euo pipefail

usage() { echo "usage: $(basename "$0") <deb|rpm> <tarball> <version> [<debian12|debian13>]" >&2; exit 2; }
[ $# -eq 3 ] || [ $# -eq 4 ] || usage
kind=$1 tarball=$2 v=$3 distro=${4:-}
[ "$kind" != deb ] || [ -n "$distro" ] || usage

# deb_changelog FILE VERSION DISTRO: prepends the entry "skycue
# (VERSION-1~debNNu1) CODENAME" to FILE, with the maintainer line and date of
# FILE's first entry (so the build stays reproducible). No devscripts needed.
deb_changelog() {
  local file=$1 v=$2 distro=$3 n code trailer
  case $distro in
    debian12) n=12 code=bookworm ;;
    debian13) n=13 code=trixie ;;
    *) echo "build-package: unknown Debian release $distro (debian12 or debian13)" >&2; return 2 ;;
  esac
  trailer=$(grep -m1 '^ -- ' "$file") || { echo "build-package: no trailer line in $file" >&2; return 1; }
  { printf 'skycue (%s-1~deb%su1) %s; urgency=medium\n\n' "$v" "$n" "$code"
    printf '  * Build for Debian %s (%s).\n\n%s\n\n' "$n" "$code" "$trailer"
    cat "$file"; } > "$file.new"
  mv "$file.new" "$file"
}

# deb_unitdir TREE DISTRO: prints the unit directory for DISTRO (debian/rules
# reads it as SKYCUE_DEB_UNITDIR) and makes TREE/debian/skycue.install name the
# unit there. Debian 12's debhelper (13.11.4) looks for units only under
# lib/systemd/system; Debian 13's finds them under usr/lib/systemd/system.
deb_unitdir() {
  local tree=$1 distro=$2 dir f
  case $distro in
    debian12) dir=/lib/systemd/system ;;
    debian13) dir=/usr/lib/systemd/system ;;
    *) echo "build-package: unknown Debian release $distro (debian12 or debian13)" >&2; return 2 ;;
  esac
  f=$tree/debian/skycue.install
  sed -i -E "s|^(usr/)?lib/systemd/system/skycued\.service$|${dir#/}/skycued.service|" "$f"
  grep -qx "${dir#/}/skycued.service" "$f" || { echo "build-package: no unit line in $f" >&2; return 1; }
  echo "$dir"
}
[ -f "$tarball" ] || { echo "build-package: no source tarball at $tarball" >&2; exit 2; }
[ -d /out ] && [ -w /out ] || { echo "build-package: /out is not a writable directory" >&2; exit 2; }
slow=${SKYCUE_TEST_SLOW_FACTOR:-1}
export SKYCUE_TEST_SLOW_FACTOR=$slow
shopt -s nullglob

case $kind in
  deb)
    mkdir -p /build
    tar -C /build -xzf "$tarball"
    cd "/build/skycue-$v"
    deb_changelog debian/changelog "$v" "$distro"
    SKYCUE_DEB_UNITDIR=$(deb_unitdir . "$distro")
    export SKYCUE_DEB_UNITDIR
    export DEB_BUILD_OPTIONS=${DEB_BUILD_OPTIONS:-parallel=$(nproc)}
    dpkg-buildpackage -b -us -uc
    debs=(/build/*.deb /build/*.ddeb)
    [ ${#debs[@]} -gt 0 ] || { echo "build-package: dpkg-buildpackage left no .deb" >&2; exit 1; }
    cp "${debs[@]}" /out/
    ;;
  rpm)
    top=/tmp/rpm
    mkdir -p "$top/SOURCES" "$top/SPECS"
    cp "$tarball" "$top/SOURCES/skycue-$v.tar.gz"
    tar -xzf "$tarball" -O "skycue-$v/packaging/rpm/skycue.spec" > "$top/SPECS/skycue.spec"
    rpmbuild -bb --define "_topdir /tmp/rpm" --define "skycue_version $v" \
      --define "skycue_slow_factor $slow" "$top/SPECS/skycue.spec"
    rpms=("$top"/RPMS/*/*.rpm)
    [ ${#rpms[@]} -gt 0 ] || { echo "build-package: rpmbuild left no .rpm" >&2; exit 1; }
    cp "${rpms[@]}" /out/
    ;;
  *) echo "build-package: unknown package kind $kind (deb or rpm)" >&2; exit 2 ;;
esac
echo "build-package: copied to /out:"
ls -1 /out
