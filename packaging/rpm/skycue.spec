# skycue for EL8 and EL9. Built from the release source tarball by
# scripts/build-package.sh, which passes the version (read from
# CMakeLists.txt) and, for slow builds, the test timeout factor:
#   rpmbuild -bb --define "skycue_version <v>" [--define "skycue_slow_factor N"]
%{!?skycue_version:%{error:define skycue_version (scripts/build-package.sh does)}}

Name:           skycue
Version:        %{skycue_version}
Release:        1%{?dist}
Summary:        ADS-B and UAT operator console and forwarding bridge
License:        Apache-2.0
URL:            https://github.com/zberry/skycue
Source0:        skycue-%{version}.tar.gz

BuildRequires:  gcc-c++ cmake >= 3.25 boost-devel zlib-devel python3 diffutils make
# A ctest talks to the daemon's web port over HTTP.
BuildRequires:  curl
# %%{_unitdir} and the systemd scriptlet macros.
%if 0%{?rhel} >= 9
BuildRequires:  systemd-rpm-macros
%else
BuildRequires:  systemd
%endif
Requires(pre):  shadow-utils
# skycue-make-maps uses curl and unzip to fetch, tiles-merge.py needs python3
# and Pillow (python3-pillow; skipped by dnf where no repository has it).
Recommends:     python3 python3-pillow curl unzip
%{?systemd_requires}

%description
skycue runs beside the reader's receiver stack (readsb for 1090 MHz,
dump978 for 978 MHz UAT) and reads the decoded traffic those programs
already publish. It does not feed, replace or configure them: no decoder
is a dependency, and nothing here edits their configuration or restarts
them.

The skycued daemon serves an offline web console with a map, a traffic
table and alerts, and sends cues to cue clients. Map tiles and
overlays are made on the machine with skycue-make-maps (GDAL through a
container, or baked on another machine and copied). The service is
neither enabled nor started on install: set RECEIVER_LAT and RECEIVER_LON
in /etc/skycue/skycue.conf first, then run "systemctl enable --now skycued".

%package devel
Summary:        skycue client library (static) and CMake package
Requires:       boost-devel

%description devel
libskycue is the static C++ client library for skycued's cue interface,
with its headers and a CMake package configuration for
find_package(skycue).

skycue runs beside the reader's receiver stack and does not feed, replace
or configure it; this package only lets a client program talk to skycued.

%prep
%autosetup -n skycue-%{version}

%build
# The CMake project is at the top of this tree (_vpath_srcdir stays at its
# default, the source directory). EL8's cmake-rpm-macros default to an
# in-source build; build out of source (EL9's default) so the build directory
# is separate. %%ctest and %%cmake_install follow.
%undefine __cmake_in_source_build
# Leaflet and ECharts come from sources/ in the source tarball (no downloads
# at build time). BUILD_SHARED_LIBS=ON from %%cmake does not touch the
# explicitly STATIC libraries.
%cmake \
    -DSKYCUE_UI_TESTS=OFF \
    -DSKYCUE_PACKAGE_CONFIG=ON \
    -DSKYCUE_SOURCE_CACHE=%{_builddir}/skycue-%{version}/sources \
    -DSKYCUE_SYSTEMD_UNITDIR=%{_unitdir} \
    -DCMAKE_INSTALL_SYSCONFDIR=%{_sysconfdir} \
    -DSKYCUE_TEST_SLOW_FACTOR=%{?skycue_slow_factor}%{!?skycue_slow_factor:1}
%cmake_build

%install
%cmake_install
install -d %{buildroot}%{_sharedstatedir}/skycue \
    %{buildroot}%{_sharedstatedir}/skycue/maps \
    %{buildroot}%{_sharedstatedir}/skycue/recordings
# Owned as %%ghost: %%post creates them empty, an upgrade never replaces them.
touch %{buildroot}%{_sharedstatedir}/skycue/allow.list %{buildroot}%{_sharedstatedir}/skycue/block.list

%check
# Serially (-j1): some tests listen on fixed local ports. %%ctest takes no -j
# of its own, so its %%{_smp_mflags} is set here. install_layout checks this
# package shape.
%define _smp_mflags -j1
%ctest

%pre
getent group skycue >/dev/null || groupadd -r skycue
getent passwd skycue >/dev/null || useradd -r -g skycue -d /var/lib/skycue -s /sbin/nologin -c "skycue daemon" skycue
exit 0

%post
%systemd_post skycued.service
# skycued stops on a missing list file; an empty list means no entries.
for f in /var/lib/skycue/allow.list /var/lib/skycue/block.list; do
    [ -e "$f" ] || install -o skycue -g skycue -m 0644 /dev/null "$f"
done
exit 0

%preun
%systemd_preun skycued.service

%postun
%systemd_postun_with_restart skycued.service

%files
%license LICENSE NOTICE
%{_bindir}/skycued
%{_bindir}/skycue-*
%{_datadir}/skycue/
%{_unitdir}/skycued.service
%dir %{_sysconfdir}/skycue
%config(noreplace) %{_sysconfdir}/skycue/skycue.conf
%dir %attr(0755,skycue,skycue) %{_sharedstatedir}/skycue
%dir %attr(0755,skycue,skycue) %{_sharedstatedir}/skycue/maps
%dir %attr(0755,skycue,skycue) %{_sharedstatedir}/skycue/recordings
%ghost %config(noreplace) %attr(0644,skycue,skycue) %{_sharedstatedir}/skycue/allow.list
%ghost %config(noreplace) %attr(0644,skycue,skycue) %{_sharedstatedir}/skycue/block.list

%files devel
%{_includedir}/skycue/
%{_libdir}/libskycue.a
%{_libdir}/cmake/skycue/

%changelog
* Wed Nov 04 2026 skycue maintainers <skycue@users.noreply.github.com> - 0.1.0-1
- First RPM package of skycue: skycue (daemon, tools, web console, systemd
  unit, packaged configuration) and skycue-devel.
