#!/usr/bin/env bash
# Builds examples/parent-project (a separate CMake project that pulls in
# skycue/ with add_subdirectory) against a copy of skycue/ taken out of the
# repository, as a parent tree that vendors it would, then runs
# `client_demo --help`. The copy proves the library reaches for nothing
# outside skycue/. Then installs libskycue from the copy with an explicit
# libdir (lib64; on Debian lib/<multiarch>, since Debian's CMake does not
# search lib64 in find_package) and builds the example again with
# CLIENT_DEMO_FIND_PACKAGE=ON against that install. Exits 77 (ctest: skipped) when no cmake is found.
#
# usage: test_parent_project.sh <skycue dir> <work dir>
# Environment: CMAKE (default: cmake on PATH; ctest passes CMAKE_COMMAND),
# CXX, SKYCUE_SMOKE_BOOST_DIR, SKYCUE_SMOKE_PREFIX_PATH (passed through so
# the compiler and Boost match the enclosing build), CMAKE_BUILD_PARALLEL_LEVEL
# (default: nproc).
set -euo pipefail
# The "built nothing else" checks below pass silently if find is missing.
command -v find >/dev/null || { echo "test_parent_project: find (findutils) not found" >&2; exit 1; }

skycue_dir=$1
work_base=$2
cmake_bin=${CMAKE:-cmake}

if ! command -v "$cmake_bin" >/dev/null 2>&1; then
    echo "SKIP: cmake not found ($cmake_bin)"
    exit 77
fi
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc 2>/dev/null || echo 2)}

mkdir -p "$work_base"
work=$(mktemp -d "$work_base/parent-project.XXXXXX")
trap 'rm -rf "$work"' EXIT

# skycue/ alone, without build trees, VCS data or local-only additions (map
# tiles, a local Leaflet). The patterns are anchored below the top directory
# so a work dir under a build* tree does not exclude the copy itself.
name=$(basename "$skycue_dir")
mkdir "$work/vendor"
# A build tree inside the source tree under another name (a Debian package
# build uses obj-*, an RPM build redhat-linux-build or similar) holds the
# work dir: leave out its top directory, or the copy would contain itself.
inner_build=()
rel=${work#"$(cd "$skycue_dir" && pwd -P)"/}
if [[ $rel != "$work" ]]; then inner_build=(--exclude="$name/${rel%%/*}"); fi
tar -C "$(dirname "$skycue_dir")" --anchored "${inner_build[@]}" \
    --exclude="$name/build*" --exclude="$name/.git" \
    --exclude="$name/www/tiles" --exclude="$name/www/vendor" \
    -cf - "$name" | tar -C "$work/vendor" -xf -
vendored=$work/vendor/$name

# tool_args: the compiler and Boost of the enclosing build. The prefix path
# is kept apart because package mode below puts the install in front of it.
tool_args=()
if [[ -n ${CXX:-} ]]; then tool_args+=("-DCMAKE_CXX_COMPILER=$CXX"); fi
if [[ -n ${SKYCUE_SMOKE_BOOST_DIR:-} ]]; then tool_args+=("-DBoost_DIR=$SKYCUE_SMOKE_BOOST_DIR"); fi
prefix_args=()
if [[ -n ${SKYCUE_SMOKE_PREFIX_PATH:-} ]]; then prefix_args+=("-DCMAKE_PREFIX_PATH=$SKYCUE_SMOKE_PREFIX_PATH"); fi
args=("-DSKYCUE_SOURCE_DIR=$vendored" "${tool_args[@]}" "${prefix_args[@]}")

# The example from the copy, so its default SKYCUE_SOURCE_DIR would also be
# the copy; it is passed explicitly anyway.
"$cmake_bin" -S "$vendored/examples/parent-project" -B "$work/build" "${args[@]}"
"$cmake_bin" --build "$work/build" -j "$jobs"

# Only libskycue may come from skycue: no daemon, tools or tests.
for t in skycued skycue-dump test_protocol minimal_consumer; do
    if find "$work/build" -name "$t" -type f | grep -q .; then
        echo "parent project built skycue's $t" >&2
        exit 1
    fi
done

out=$("$work/build/client_demo" --help)
echo "$out"
[[ $out == usage:* ]] || { echo "client_demo --help printed no usage" >&2; exit 1; }
echo "parent project: client_demo built against a copy of skycue/ and ran"

# Package mode: libskycue installed from the copy (an explicit libdir that is
# not GNUInstallDirs' default for this prefix, proving it is honoured), then the parent project through
# find_package(skycue). The install comes first in CMAKE_PREFIX_PATH, ahead
# of the enclosing build's, so no other skycue install can be picked up.
# Debian's CMake searches lib/<multiarch> but not lib64 (it is not a Debian
# library directory), so there the libdir is lib/<multiarch>. The package
# directory is passed as skycue_DIR only when no triplet could be derived.
libdir=lib64
pkg_dir_args=()
if [[ -e /etc/debian_version ]]; then
    # gcc prints the triplet; clang prints nothing, so ask dpkg, and without either use plain lib
    # (still a Debian library directory; the configure below then gets skycue_DIR explicitly).
    multiarch=$("${CXX:-c++}" -print-multiarch 2>/dev/null || true)
    [[ -n $multiarch ]] || multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)
    if [[ -n $multiarch ]]; then libdir=lib/$multiarch; else libdir=lib; pkg_dir_args=("-Dskycue_DIR:PATH=$work/prefix/lib/cmake/skycue"); fi
fi
"$cmake_bin" -S "$vendored" -B "$work/lib" -DSKYCUE_BUILD_DAEMON=OFF -DSKYCUE_BUILD_TESTS=OFF \
    -DSKYCUE_BUILD_EXAMPLES=OFF -DCMAKE_INSTALL_LIBDIR=$libdir "${tool_args[@]}" "${prefix_args[@]}"
"$cmake_bin" --build "$work/lib" -j "$jobs"
"$cmake_bin" --install "$work/lib" --prefix "$work/prefix"
test -f "$work/prefix/$libdir/cmake/skycue/skycueConfig.cmake" || { echo "libskycue config not under $libdir" >&2; exit 1; }
test -f "$work/prefix/$libdir/libskycue.a" || { echo "libskycue.a not under $libdir" >&2; exit 1; }
test -f "$work/prefix/include/skycue/version.hpp" || { echo "version.hpp not installed" >&2; exit 1; }
test -f "$work/prefix/include/skycue/client.hpp" || { echo "client.hpp not installed" >&2; exit 1; }
stray=$(find "$work/prefix" -path "$work/prefix/include" -prune -o -path "$work/prefix/$libdir" -prune -o -type f -print)
if [[ -n $stray ]]; then echo "libskycue installed files outside include/ and $libdir/: $stray" >&2; exit 1; fi
"$cmake_bin" -S "$vendored/examples/parent-project" -B "$work/pkg" -DCLIENT_DEMO_FIND_PACKAGE=ON \
    "-DCMAKE_PREFIX_PATH=$work/prefix${SKYCUE_SMOKE_PREFIX_PATH:+;$SKYCUE_SMOKE_PREFIX_PATH}" "${pkg_dir_args[@]}" "${tool_args[@]}"
grep -qx "skycue_DIR:PATH=$work/prefix/$libdir/cmake/skycue" "$work/pkg/CMakeCache.txt" \
    || { echo "find_package did not use the install" >&2; exit 1; }
"$cmake_bin" --build "$work/pkg" -j "$jobs"
# find_package mode compiles no skycue sources: the archive comes from the install.
if find "$work/pkg" -name 'protocol.cpp.o' -o -name 'libskycue.a' | grep -q .; then
    echo "package mode built libskycue itself" >&2
    exit 1
fi
out=$("$work/pkg/client_demo" --help)
[[ $out == usage:* ]] || { echo "package-mode client_demo printed no usage" >&2; exit 1; }
echo "parent project: client_demo built against an installed libskycue"
