#!/usr/bin/env bash
# Regenerates raw/uat-sample.golden.jsonl.gz and raw/uat-stratux.golden.jsonl.gz: what
# dump978-fa makes of the UAT sample and of the Stratux log, one run per file.
#
#   gen_uat_golden.sh DUMP978 IQGEN SAMPLE_GZ OUT_GZ
#
# SAMPLE_GZ lines are "-HEX;..." or "TIMESTAMP,-HEX;..." (the Stratux log); uplinks and
# other lines are skipped.
#
# DUMP978 is the dump978-fa binary, IQGEN the built skycue-iqgen; both are
# passed in, nothing here knows where they live. The downlink lines of the
# sample are modulated to a temporary CU8 file (about 80 KB per frame, deleted
# on exit), demodulated again by dump978-fa, and gen_uat_golden.py pairs every
# raw line with the JSON object of the same receive time.
set -euo pipefail

if [ "$#" -ne 4 ]; then
    echo "usage: $0 DUMP978 IQGEN SAMPLE_GZ OUT_GZ" >&2
    exit 2
fi
dump978=$1 iqgen=$2 sample=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
# A failed run keeps the tools' logs (tails on stderr; files in the
# current directory only with GEN_UAT_GOLDEN_KEEP_LOGS=1).
cleanup() {
    status=$?
    if [ "$status" -ne 0 ]; then
        for f in iqgen.log dump978.log; do
            if [ -s "$work/$f" ]; then
                echo "--- $f (tail) ---" >&2
                tail -n 20 "$work/$f" >&2
                if [ "${GEN_UAT_GOLDEN_KEEP_LOGS:-}" = 1 ]; then
                    cp "$work/$f" "./gen_uat_golden.$f" 2>/dev/null || true
                fi
            fi
        done
    fi
    rm -rf "$work"
}
trap cleanup EXIT

gzip -dc "$sample" | grep -E '^([0-9]+,)?-' | sed -E 's/^[0-9]+,//' >"$work/downlinks.txt" || true
if [ ! -s "$work/downlinks.txt" ]; then
    echo "no downlink lines in $sample" >&2
    exit 1
fi
"$iqgen" uat "$work/downlinks.txt" -o "$work/t.cu8" >"$work/iqgen.log"
"$dump978" --file "$work/t.cu8" --format CU8 --raw-stdout --json-stdout >"$work/out.txt" 2>"$work/dump978.log" || true
rm -f "$work/t.cu8"
python3 -I "$here/gen_uat_golden.py" "$work/out.txt" "$out"
