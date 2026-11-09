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
. "$here/../lib/test_common.sh"
test_init gen_raw_golden

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
    --net-json-port-include-noposition --quiet >"$tmp/readsb.log" 2>&1 &
bg_pids+=($!)
readsb_pid=$!
# Wait (up to 10 s) for readsb to bind its JSON port.
wait_port() {
    python3 -I - "$1" <<'PY'
import socket, sys, time
port = int(sys.argv[1])
end = time.time() + 10
while time.time() < end:
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
        sys.exit(0)
    except OSError:
        time.sleep(0.2)
sys.exit(1)
PY
}
wait_port "$p_json" || { echo "readsb did not bind its JSON port" >&2; cat "$tmp/readsb.log" >&2; exit 1; }
kill -0 "$readsb_pid" || { cat "$tmp/readsb.log" >&2; exit 1; }

python3 -I "$here/gen_raw_golden.py" collect "$p_json" "$tmp/lines.jsonl" &
bg_pids+=($!)
collector_pid=$!
sleep 1
kill -0 "$collector_pid" || { echo "collector exited early" >&2; exit 1; }

wait_port "$p_raw" || { echo "readsb did not bind its raw input port" >&2; exit 1; }
"$rawplay" --raw-in "127.0.0.1:$p_raw" --speed 1 "$csv"
sleep 2
kill -0 "$collector_pid" || { echo "collector died during playback" >&2; exit 1; }
kill "$readsb_pid"
wait "$readsb_pid" 2>/dev/null || true
wait "$collector_pid" 2>/dev/null || true
bg_pids=()

series=()
[ -n "${GOLDEN_SERIES_DIR:-}" ] && series=(--dump-series "$GOLDEN_SERIES_DIR")
python3 -I "$here/gen_raw_golden.py" reduce "$tmp/lines.jsonl" "$out" ${series[@]+"${series[@]}"}
n=$(python3 -I -c 'import gzip,sys; print(sum(1 for _ in gzip.open(sys.argv[1], "rt")))' "$out")
if [ "$n" -lt 100 ]; then
    echo "golden has only $n addresses (expected at least 100)" >&2
    exit 1
fi
