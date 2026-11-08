#!/usr/bin/env bash
# Regenerates raw/uat-synthetic.golden.jsonl.gz and raw/uat-synthetic.expect.jsonl.gz: the
# synthetic UAT matrix (frames built with tests/uat_test_support.hpp's encodeUat), what
# dump978-fa makes of it, and the values built into each frame.
#
#   gen_uat_synthetic.sh DUMP978 IQGEN OUT_GZ EXPECT_GZ [UAT_SYNTH]
#
# UAT_SYNTH is the built test helper (default: build/tests/uat_synth under the repository).
# uat_synth prints the "-HEX;" lines; gen_uat_golden.sh runs them through skycue-iqgen and
# dump978-fa (temporary CU8, deleted on exit) and pairs them. DUMP978 and IQGEN are arguments;
# nothing here knows where they live.
set -euo pipefail

if [ "$#" -lt 4 ] || [ "$#" -gt 5 ]; then
    echo "usage: $0 DUMP978 IQGEN OUT_GZ EXPECT_GZ [UAT_SYNTH]" >&2
    exit 2
fi
dump978=$1 iqgen=$2 out=$3 expect=$4
here=$(cd "$(dirname "$0")" && pwd)
synth=${5:-$here/../../build/tests/uat_synth}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$synth" --expect "$work/expect.jsonl" >"$work/frames.txt"
gzip -n -c "$work/frames.txt" >"$work/frames.txt.gz"
gzip -n -c "$work/expect.jsonl" >"$expect"
"$here/gen_uat_golden.sh" "$dump978" "$iqgen" "$work/frames.txt.gz" "$out"
