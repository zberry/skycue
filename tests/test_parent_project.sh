#!/usr/bin/env bash
# Builds examples/parent-project (a separate CMake project that pulls in
# skycue/ with add_subdirectory) against a copy of skycue/ taken out of the
# repository, as a parent tree that vendors it would, then runs
# `client_demo --help`. The copy proves the library reaches for nothing
# outside skycue/. Exits 77 (ctest: skipped) when no cmake is found.
#
# usage: test_parent_project.sh <skycue dir> <work dir>
# Environment: CMAKE (default: cmake on PATH; ctest passes CMAKE_COMMAND),
# CXX, SKYCUE_SMOKE_BOOST_DIR, SKYCUE_SMOKE_PREFIX_PATH (passed through so
# the compiler and Boost match the enclosing build), CMAKE_BUILD_PARALLEL_LEVEL
# (default: nproc).
set -euo pipefail

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
tar -C "$(dirname "$skycue_dir")" --anchored \
    --exclude="$name/build*" --exclude="$name/.git" \
    --exclude="$name/www/tiles" --exclude="$name/www/vendor" \
    -cf - "$name" | tar -C "$work/vendor" -xf -
vendored=$work/vendor/$name

args=("-DSKYCUE_SOURCE_DIR=$vendored")
if [[ -n ${CXX:-} ]]; then args+=("-DCMAKE_CXX_COMPILER=$CXX"); fi
if [[ -n ${SKYCUE_SMOKE_BOOST_DIR:-} ]]; then args+=("-DBoost_DIR=$SKYCUE_SMOKE_BOOST_DIR"); fi
if [[ -n ${SKYCUE_SMOKE_PREFIX_PATH:-} ]]; then args+=("-DCMAKE_PREFIX_PATH=$SKYCUE_SMOKE_PREFIX_PATH"); fi

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
