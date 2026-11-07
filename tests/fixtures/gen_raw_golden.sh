#!/usr/bin/env bash
# Regenerates raw/modes-120s.golden.jsonl.gz: what readsb makes of the 120 s Mode S capture.
#
#   gen_raw_golden.sh READSB RAWPLAY CSV_GZ OUT_GZ
#
# READSB is the readsb binary (the superbuild pins 3.16.21), RAWPLAY the built
# skycue-rawplay; both are passed in, nothing here knows where they live.
# readsb runs without --lat/--lon, so it decodes positions globally only, the
# same rule the decoder test follows. The JSON port's lines are collected and
# reduced by gen_raw_golden.py.
set -euo pipefail

if [ "$#" -ne 4 ]; then
    echo "usage: $0 READSB RAWPLAY CSV_GZ OUT_GZ" >&2
    exit 2
fi
readsb=$1 rawplay=$2 csv=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
pids=()
cleanup() {
    for p in "${pids[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null || true; done
    rm -rf "$work"
}
trap cleanup EXIT

# Two free ports: raw input and JSON output.
read -r p_raw p_json < <(python3 -I - <<'PY'
import socket
s = [socket.socket() for _ in range(2)]
for x in s:
    x.bind(("127.0.0.1", 0))
print(*(x.getsockname()[1] for x in s))
PY
)

"$readsb" --net-only --net-ri-port "$p_raw" --net-json-port "$p_json" \
    --net-json-port-include-noposition --quiet >"$work/readsb.log" 2>&1 &
pids+=($!)
readsb_pid=$!
sleep 2
kill -0 "$readsb_pid" || { cat "$work/readsb.log" >&2; exit 1; }

python3 -I "$here/gen_raw_golden.py" collect "$p_json" "$work/lines.jsonl" &
pids+=($!)
collector_pid=$!
sleep 1

"$rawplay" --raw-in "127.0.0.1:$p_raw" --speed 1 "$csv"
sleep 2
kill "$readsb_pid"
wait "$readsb_pid" 2>/dev/null || true
wait "$collector_pid" 2>/dev/null || true
pids=()

python3 -I "$here/gen_raw_golden.py" reduce "$work/lines.jsonl" "$out"
