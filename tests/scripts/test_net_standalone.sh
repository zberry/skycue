#!/usr/bin/env bash
# net/ alone: a copy of only the net/ directory (without cmake/CompilerWarnings.cmake
# or anything else of the tree) is added to a separate project with
# add_subdirectory and linked as skycue::net. The program parses an endpoint
# and must print "tcp_listen 0.0.0.0:30005". The copy must also contain no
# include that reaches outside net/. Exits 77 (ctest: skipped) without cmake.
#
# usage: test_net_standalone.sh [<skycue dir> [<work dir>]]
# Environment: CMAKE, CXX, SKYCUE_SMOKE_BOOST_DIR, SKYCUE_SMOKE_PREFIX_PATH,
# CMAKE_BUILD_PARALLEL_LEVEL (as test_parent_project.sh).
set -euo pipefail

here=$(cd "$(dirname "$0")/../.." && pwd)
skycue_dir=${1:-$here}
cmake_bin=${CMAKE:-cmake}

if ! command -v "$cmake_bin" >/dev/null 2>&1; then
    echo "SKIP: cmake not found ($cmake_bin)"
    exit 77
fi
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc 2>/dev/null || echo 2)}

if [[ -n ${2:-} ]]; then mkdir -p "$2"; work=$(mktemp -d "$2/net-standalone.XXXXXX"); else work=$(mktemp -d); fi
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/src"
cp -r "$skycue_dir/net" "$work/src/net"
rm -rf "$work/src/net/build"* 2>/dev/null || true

# Includes: "..." only inside net/ (relative to the including file or the
# include directory), skycue/ only skycue/net/.
bad=$(grep -rnE '#[[:space:]]*include[[:space:]]*"' "$work/src/net" |
      grep -vE '#[[:space:]]*include[[:space:]]*"(skycue/net/|[A-Za-z0-9_]+\.hpp")' || true)
bad2=$(grep -rnE '#[[:space:]]*include[[:space:]]*[<"]skycue/' "$work/src/net" |
       grep -vE '[<"]skycue/net/' || true)
if [[ -n $bad$bad2 ]]; then
    echo "net/ includes something outside net/:" >&2
    echo "$bad$bad2" >&2
    exit 1
fi

args=("-DSKYCUE_NET_DIR=$work/src/net" "-DSKYCUE_NET_BUILD_TESTS=OFF")
if [[ -n ${CXX:-} ]]; then args+=("-DCMAKE_CXX_COMPILER=$CXX"); fi
if [[ -n ${SKYCUE_SMOKE_BOOST_DIR:-} ]]; then args+=("-DBoost_DIR=$SKYCUE_SMOKE_BOOST_DIR"); fi
if [[ -n ${SKYCUE_SMOKE_PREFIX_PATH:-} ]]; then args+=("-DCMAKE_PREFIX_PATH=$SKYCUE_SMOKE_PREFIX_PATH"); fi

"$cmake_bin" -S "$here/tests/scripts/net-standalone" -B "$work/build" "${args[@]}"
"$cmake_bin" --build "$work/build" -j "$jobs"

out=$("$work/build/net_standalone")
echo "$out"
[[ $out == "tcp_listen 0.0.0.0:30005" ]] || { echo "unexpected output: $out" >&2; exit 1; }
echo "net standalone: skycue::net built from a copy of net/ alone and ran"
