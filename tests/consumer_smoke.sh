#!/usr/bin/env bash
# Builds tests/consumer-smoke against libskycue both ways a consumer can use
# it, and runs the result:
#   1. subdir:  add_subdirectory(libskycue); the build is then installed to a
#               temporary prefix, which also checks the install/export rules;
#   2. package: find_package(skycue) against that prefix;
#   3. shared:  add_subdirectory with SKYCUE_INSTALL=OFF, libskycue linked
#               into a SHARED library (proves it is position-independent);
#               installing that build must not install libskycue.
#
# usage: consumer_smoke.sh <libskycue dir> <consumer-smoke dir> <work dir>
# Environment: CMAKE (default: cmake), CXX, SKYCUE_SMOKE_PREFIX_PATH and
# SKYCUE_SMOKE_BOOST_DIR (passed through so Boost is found as in the parent build).
set -euo pipefail

lib_dir=$1
consumer_dir=$2
work_base=$3
cmake_bin=${CMAKE:-cmake}

mkdir -p "$work_base"
work=$(mktemp -d "$work_base/consumer-smoke.XXXXXX")
trap 'rm -rf "$work"' EXIT

common=(-DCMAKE_BUILD_TYPE=Release)
if [[ -n ${CXX:-} ]]; then common+=("-DCMAKE_CXX_COMPILER=$CXX"); fi
if [[ -n ${SKYCUE_SMOKE_BOOST_DIR:-} ]]; then common+=("-DBoost_DIR=$SKYCUE_SMOKE_BOOST_DIR"); fi
prefix_path=$work/prefix
if [[ -n ${SKYCUE_SMOKE_PREFIX_PATH:-} ]]; then prefix_path="$prefix_path;$SKYCUE_SMOKE_PREFIX_PATH"; fi

echo "== subdir mode"
"$cmake_bin" -S "$consumer_dir" -B "$work/subdir" "${common[@]}" \
    -DSKYCUE_CONSUMER_MODE=subdir "-DSKYCUE_LIB_DIR=$lib_dir"
"$cmake_bin" --build "$work/subdir" -j 4
"$work/subdir/consumer"

echo "== install libskycue to a temporary prefix"
"$cmake_bin" --install "$work/subdir" --prefix "$work/prefix"
# The library directory GNUInstallDirs chose for this platform (lib, lib64
# or lib/<multiarch>), as libskycue's configure cached it.
libdir=$(sed -n 's/^CMAKE_INSTALL_LIBDIR:[A-Z]*=//p' "$work/subdir/CMakeCache.txt")
[[ -n $libdir ]] || { echo "no CMAKE_INSTALL_LIBDIR in the subdir build's cache" >&2; exit 1; }
echo "libdir: $libdir"
test -f "$work/prefix/$libdir/cmake/skycue/skycueConfig.cmake"
test -f "$work/prefix/$libdir/cmake/skycue/skycueConfigVersion.cmake"
test -f "$work/prefix/$libdir/libskycue.a"
test -f "$work/prefix/include/skycue/client.hpp"
test -f "$work/prefix/include/skycue/version.hpp"

echo "== package mode"
"$cmake_bin" -S "$consumer_dir" -B "$work/package" "${common[@]}" \
    -DSKYCUE_CONSUMER_MODE=package "-DCMAKE_PREFIX_PATH=$prefix_path"
grep -qx "skycue_DIR:PATH=$work/prefix/$libdir/cmake/skycue" "$work/package/CMakeCache.txt"
"$cmake_bin" --build "$work/package" -j 4
"$work/package/consumer"

echo "== shared mode (SKYCUE_INSTALL=OFF)"
"$cmake_bin" -S "$consumer_dir" -B "$work/shared" "${common[@]}" \
    -DSKYCUE_CONSUMER_MODE=shared "-DSKYCUE_LIB_DIR=$lib_dir" -DSKYCUE_INSTALL=OFF
"$cmake_bin" --build "$work/shared" -j 4
"$work/shared/consumer"
"$cmake_bin" --install "$work/shared" --prefix "$work/shared-prefix"
test -x "$work/shared-prefix/bin/consumer"
if [[ -e $work/shared-prefix/include/skycue || -e $work/shared-prefix/$libdir/cmake/skycue ]]; then
    echo "SKYCUE_INSTALL=OFF still installed libskycue" >&2
    exit 1
fi

echo "consumer smoke: all three modes ok"
