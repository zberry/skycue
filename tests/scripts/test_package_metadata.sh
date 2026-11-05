#!/usr/bin/env bash
# Licence and package metadata: one licence, one version, one public identity, nothing private.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
fail() { echo "FAIL: test_package_metadata: $*"; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
[ "$(sha256sum < "$here/LICENSE" | cut -d' ' -f1)" = cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30 ] ||
  fail "LICENSE is not the Apache-2.0 text"
grep -q "Apache License, Version 2.0" "$here/NOTICE" || fail "NOTICE does not state skycue's licence"
for w in Leaflet ECharts D3 "Natural Earth"; do grep -q "$w" "$here/NOTICE" || fail "NOTICE lacks $w"; done
# The path CMakeLists.txt installs the D3 licence to.
grep -q "www/vendor/echarts/licenses/LICENSE-d3" "$here/NOTICE" || fail "NOTICE does not name the installed LICENSE-d3 path"
# deb822 fields with their continuation lines joined, one field per line.
unfold() { awk '/^[ \t]/ { sub(/^[ \t]+/, " "); buf = buf $0; next } { if (buf != "") print buf; buf = $0 } END { if (buf != "") print buf }' "$1"; }
v=$(sed -n 's/^project(skycue VERSION \([0-9.]*\).*/\1/p' "$here/CMakeLists.txt")
[ -n "$v" ] || fail "no version in CMakeLists.txt"
id='skycue maintainers <skycue@users.noreply.github.com>'
[ -f "$here/debian/control" ] || fail "debian/control missing"
# The committed changelog's top entry is <v>-1. A package build prepends a <v>-1~debNNuM
# entry in its own copy of the tree (scripts/build-package.sh), so that copy passes too.
revs=$(sed -n 's/^skycue (\([^)]*\)).*/\1/p' "$here/debian/changelog" | head -n 2 | tr '\n' ' ')
case $revs in
  "$v-1 "*) ;;
  "$v-1~deb"[0-9]*"u"[0-9]*" $v-1 ") printf '%s\n' "${revs%% *}" | grep -Eqx "$v-1~deb[0-9]+u[0-9]+" ||
    fail "debian/changelog: bad build revision ${revs%% *}" ;;
  *) fail "debian/changelog is not $v-1 (or a $v-1~debNNuM build entry above it): $revs" ;;
esac
[ "$(grep -m1 '^ -- ' "$here/debian/changelog" | sed 's/^ -- \(.*>\)  .*/\1/')" = "$id" ] || fail "changelog trailer identity"
grep -qxF "Maintainer: $id" "$here/debian/control" || fail "Maintainer"
grep -q '^Homepage:' "$here/debian/control" && fail "Homepage must be omitted"
grep -qx 'Package: skycue' "$here/debian/control" && grep -qx 'Package: libskycue-dev' "$here/debian/control" || fail "package names"
grep -qx 'Suggests: readsb, dump978-fa, gdal-bin, python3-gdal' "$here/debian/control" || fail "Suggests"
# skycue-make-maps uses curl and unzip to fetch, tiles-merge.py needs python3 and Pillow.
grep -qx 'Recommends: python3, python3-pil, curl, unzip' "$here/debian/control" || fail "Recommends"
grep -qx 'NOTICE' "$here/debian/skycue.docs" 2>/dev/null || fail "debian/skycue.docs does not ship NOTICE"
# Not started on a fresh install; an upgrade try-restarts a running daemon.
grep -qE '^\s+dh_installsystemd --no-start --restart-after-upgrade$' "$here/debian/rules" ||
  fail "rules: dh_installsystemd --no-start --restart-after-upgrade"
ctl=$(unfold "$here/debian/control")
printf '%s\n' "$ctl" | grep -Eq '^(Pre-)?Depends:.*(readsb|dump978|dump1090)' && fail "depends on a decoder"
printf '%s\n' "$ctl" | grep -E '^Build-Depends:' | grep -Eq '(^|[ ,])curl([ ,]|$)' || fail "Build-Depends lacks curl (a ctest uses it)"
grep -qx '3.0 (quilt)' "$here/debian/source/format" || fail "debian/source/format"
# The unit dir comes from SKYCUE_DEB_UNITDIR (default /usr/lib/systemd/system, Debian 13);
# build-package-inner.sh sets /lib/systemd/system for Debian 12, whose debhelper looks
# only there, and rewrites the unit line of skycue.install in its copy of the tree.
grep -qx 'SKYCUE_DEB_UNITDIR ?= /usr/lib/systemd/system' "$here/debian/rules" &&
  grep -qF -- '-DSKYCUE_SYSTEMD_UNITDIR=$(SKYCUE_DEB_UNITDIR)' "$here/debian/rules" || fail "rules: unit dir"
grep -Eqx '(usr/)?lib/systemd/system/skycued.service' "$here/debian/skycue.install" || fail "skycue.install: unit"
# prerm stops skycued on remove only (debhelper adds no stop for a --no-start unit), and only
# where systemd runs.
pr=$here/debian/skycue.prerm
[ -x "$pr" ] || fail "debian/skycue.prerm missing or not executable"
grep -qx '#DEBHELPER#' "$pr" || fail "prerm lacks #DEBHELPER#"
sh -n "$pr" || fail "prerm: sh -n"
grep -q '/run/systemd/system' "$pr" || fail "prerm not guarded by /run/systemd/system"
mkdir -p "$tmp/pbin"
printf '#!/bin/sh\necho "$*" >> "%s/calls"\n' "$tmp" > "$tmp/pbin/deb-systemd-invoke"; chmod +x "$tmp/pbin/deb-systemd-invoke"
for a in remove upgrade deconfigure failed-upgrade; do
  : > "$tmp/calls"
  PATH=$tmp/pbin:$PATH sh "$pr" "$a" 2.0-1 >/dev/null 2>&1 || fail "prerm $a failed"
  if [ "$a" = remove ] && [ -d /run/systemd/system ]; then
    grep -qx 'stop skycued.service' "$tmp/calls" || fail "prerm remove does not stop skycued"
  else
    [ -s "$tmp/calls" ] && fail "prerm $a ran deb-systemd-invoke: $(cat "$tmp/calls")"
  fi
done
printf '%s\n' "$ctl" | grep -E '^Build-Depends:' | grep -Eq '(^|[ ,])(pkgconf|systemd-dev)([ ,]|$)' && fail "unused Build-Depends (pkgconf, systemd-dev)"
for o in -DCMAKE_INSTALL_SYSCONFDIR=/etc -DSKYCUE_PACKAGE_CONFIG=ON '-DSKYCUE_SOURCE_CACHE=$(CURDIR)/sources'; do
  grep -qF -- "$o" "$here/debian/rules" || fail "rules lacks $o"
done
grep -q 'dh_auto_test --no-parallel' "$here/debian/rules" || fail "rules: ctest must run serially"
unfold "$here/debian/control" | grep -q 'then run "systemctl enable --now skycued"' || fail "control: enable --now"
for p in /var/lib/skycue/maps /var/lib/skycue/recordings allow.list block.list; do
  grep -qF "$p" "$here/debian/skycue.postinst" || fail "postinst does not create $p"
done
grep -qx 'etc/skycue/skycue.conf' "$here/debian/skycue.install" || fail "skycue.install: conf"
for l in Apache-2.0 BSD-2-Clause BSD-3-Clause BSD-2-Clause-Patent; do grep -qx "License: $l" "$here/debian/copyright" || fail "copyright lacks $l"; done
# Nothing private: the only e-mail address in the packaging is the project's.
others=$(grep -rhoE '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[a-z]{2,}' "$here/debian" "$here/packaging" 2>/dev/null | sort -u | grep -vx 'skycue@users.noreply.github.com')
[ -z "$others" ] || fail "other addresses: $others"
grep -rqE '/home/|session_' "$here/debian" "$here/packaging" && fail "a local path or session link"
s=$here/packaging/rpm/skycue.spec
[ -f "$s" ] || fail "rpm spec missing"
grep -qx 'Version: *%{skycue_version}' "$s" || fail "spec Version must come from skycue_version"
grep -qx 'License: *Apache-2.0' "$s" || fail "spec License"
grep -q '^URL:' "$s" && fail "URL must be omitted"
sed -n '/^%changelog/,$p' "$s" | sed -n '2p' | grep -qF "$id - $v-1" || fail "rpm %changelog is not $v-1 by $id"
grep -q '%config(noreplace) %{_sysconfdir}/skycue/skycue.conf' "$s" || fail "conf not %config(noreplace)"
grep -Eq '^Requires:.*(readsb|dump978|dump1090)' "$s" && fail "requires a decoder"
grep -Eq '^BuildRequires:.*(^|[ ,:])curl([ ,]|$)' "$s" || fail "spec BuildRequires lacks curl (a ctest uses it)"
# EL8's cmake-rpm-macros define __cmake_in_source_build: %cmake then passes no
# -S/-B and configures the top-level superbuild instead of _vpath_srcdir.
sed -n '/^%build/,/^%cmake /p' "$s" | grep -qx '%undefine __cmake_in_source_build' ||
  fail "spec %build must undefine __cmake_in_source_build before %cmake (EL8 builds in-source otherwise)"
sed -n '/^%check/,/^%pre/p' "$s" | grep -qx '%define _smp_mflags -j1' && grep -qx '%ctest' "$s" ||
  fail "spec %check must run ctest serially (-j1)"
for f in allow.list block.list; do grep -q "/var/lib/skycue/$f" "$s" || fail "spec %post does not create $f"; done
for f in allow.list block.list; do
  grep -qx "%ghost %config(noreplace) %attr(0644,skycue,skycue) %{_sharedstatedir}/skycue/$f" "$s" || fail "spec: $f not %ghost %config(noreplace)"
done
grep -qx '%systemd_postun_with_restart skycued.service' "$s" || fail "spec: upgrade must try-restart skycued"
for r in python3 python3-pillow curl unzip; do
  grep -Eq "^Recommends:.*(^|[ ,:])$r([ ,]|\$)" "$s" || fail "spec Recommends lacks $r"
done
echo PASS
