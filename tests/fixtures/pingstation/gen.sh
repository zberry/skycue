#!/bin/sh
# Regenerates traffic-1090.json, status.json and status-curly-quotes.json in
# this directory with the project's own simulator.
#
#   gen.sh <skycue-pingsim> <skycue-fixtures-dir>
#
# <skycue-fixtures-dir> is tests/fixtures (it holds readsb-sample.jsonl; the
# outputs go to its pingstation/ subdirectory). python3 is the only other tool.
#
# The generator command, run once with a UDP listener on a free local port:
#
#   skycue-pingsim --udp 127.0.0.1:PORT --as json --speed 0 --status-interval 1 \
#       --status-lat 38.9 --status-lon -77.0 tests/fixtures/readsb-sample.jsonl
#
# The simulator stamps every datagram with the send time, so a plain run is not
# reproducible. The first aircraft datagram and the first status datagram are
# therefore post-processed, and the result is asserted:
#   - timeStamp is fixed (aircraft 2026-11-02T11:59:58Z, status
#     2026-11-02T11:59:58.250Z), so the tests can compare absolute times;
#   - the aircraft gets timeOfReceptionS (the GPS-epoch counter of its
#     timeStamp, no leap-second correction), timeOfReceptionNS as a decimal
#     STRING, and detail.rawData (a 14-byte Mode S frame, made-up content, only
#     its length and the address bytes matter);
#   - the callsign gets a trailing space (a real device pads it to 8 characters);
#   - status-curly-quotes.json is status.json with the ASCII quotes around the
#     key "status" replaced by U+201C/U+201D; the parser must reject it.
# Everything else (address, position, altitudes, speed, squawk, status
# fields) is what skycue-pingsim produced from the first readsb-sample.jsonl
# aircraft; all values are synthetic.
#
# The CAT021 vector of tests/test_cat021.cpp (kSampleBlock) comes from the same
# input file with the generator command
#
#   skycue-pingsim --udp 127.0.0.1:PORT --as cat021 --speed 0 tests/fixtures/readsb-sample.jsonl
#
# whose first datagram is one 51-byte block. This script runs it too and prints
# that block as hex. Only the I021/073 time-of-message-reception bytes depend on
# the send time, so the printed hex equals kSampleBlock except for those 3 bytes
# (the test fixes the values of the run that produced it).
set -eu

if [ $# -ne 2 ]; then
    echo "usage: $0 <skycue-pingsim> <skycue-fixtures-dir>" >&2
    exit 2
fi
SIM=$1
FIX=$2
OUT=$FIX/pingstation
[ -x "$SIM" ] || { echo "$0: $SIM is not executable" >&2; exit 2; }
[ -f "$FIX/readsb-sample.jsonl" ] || { echo "$0: $FIX/readsb-sample.jsonl missing" >&2; exit 2; }
mkdir -p "$OUT"

exec python3 -I - "$SIM" "$FIX/readsb-sample.jsonl" "$OUT" <<'PY'
import calendar, json, socket, subprocess, sys

sim, sample, out = sys.argv[1:4]
GPS_EPOCH = 315964800

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", 0))
sock.settimeout(10)
port = sock.getsockname()[1]
proc = subprocess.Popen([sim, "--udp", "127.0.0.1:%d" % port, "--as", "json", "--speed", "0",
                         "--status-interval", "1", "--status-lat", "38.9", "--status-lon", "-77.0",
                         sample], stderr=subprocess.DEVNULL)
aircraft = status = None
try:
    while aircraft is None or status is None:
        data, _ = sock.recvfrom(65535)
        doc = json.loads(data.decode("utf-8"))
        if "status" in doc and status is None:
            status = doc
        elif "icaoAddress" in doc and aircraft is None:
            aircraft = doc
finally:
    proc.kill()
    proc.wait()

# ---- CAT021 block (printed, not stored) ----
sock.close()
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", 0))
sock.settimeout(10)
port = sock.getsockname()[1]
proc = subprocess.Popen([sim, "--udp", "127.0.0.1:%d" % port, "--as", "cat021", "--speed", "0", sample],
                        stderr=subprocess.DEVNULL)
try:
    block, _ = sock.recvfrom(65535)
finally:
    proc.kill()
    proc.wait()
assert block[0] == 0x15 and int.from_bytes(block[1:3], "big") == len(block)
print("cat021 first block (%d bytes): %s" % (len(block), block.hex(" ")))

# ---- aircraft ----
stamp = "2026-11-02T11:59:58Z"
unix = calendar.timegm((2026, 11, 2, 11, 59, 58))
aircraft["timeStamp"] = stamp
aircraft["timeOfReceptionS"] = unix - GPS_EPOCH
aircraft["timeOfReceptionNS"] = "412907153"  # a string on purpose
assert isinstance(aircraft["timeOfReceptionNS"], str)
assert aircraft["callsign"] == aircraft["callsign"].strip() and len(aircraft["callsign"]) < 8
aircraft["callsign"] += " "
aircraft.setdefault("detail", {})["rawData"] = "8d" + aircraft["icaoAddress"].lower() + "5821c2a65e8f1b30d47a"
assert len(aircraft["detail"]["rawData"]) == 28
assert aircraft["utcSync"] == 1 and aircraft["trafficSource"] == 0
traffic = json.dumps(aircraft, separators=(",", ":"), ensure_ascii=True) + "\n"
assert '"callsign":"' in traffic and ' ",' in traffic

# ---- status ----
status["status"]["timeStamp"] = "2026-11-02T11:59:58.250Z"
text = json.dumps(status, separators=(",", ":"), ensure_ascii=True) + "\n"
curly = text.replace('"status"', "“status”", 1)
assert curly != text
json.loads(text)
try:
    json.loads(curly)
    raise SystemExit("curly-quote file parses as JSON")
except ValueError:
    pass

for name, body in (("traffic-1090.json", traffic), ("status.json", text), ("status-curly-quotes.json", curly)):
    with open("%s/%s" % (out, name), "w", encoding="utf-8", newline="\n") as f:
        f.write(body)
    print("wrote pingstation/%s (%d bytes)" % (name, len(body.encode("utf-8"))))
PY
