#!/usr/bin/env python3
"""Generate received-tg30.jsonl: tg30.978's uplinks as a received-stream recording.

Deterministic; run from anywhere:

    python3 tests/data/fisb/gen_received_tg30.py [--test-data DIR]

--test-data DIR reads DIR/fisb/tg30.978 and writes DIR/fisb/received-tg30.jsonl;
the default is the test data directory this script is in.

Input (committed): tg30.978, dump978-fa raw lines
`+<hex>;rs=<n>;rssi=<dBFS>;t=<unix seconds>.<ms>;` (the `#` comment lines are
skipped).

Output (next to it): received-tg30.jsonl, one "uat_uplink" record per
uplink, exactly as skycued's Recorder writes it ({"time","type","uplink_hex"}),
at the line's own `t=` time (October 2020, already in time order). The same
conversion as tests/test_fisb_web.cpp's tg30AsRecording. tg30 carries real
NEXRAD regional and CONUS reflectivity, lightning, icing, turbulence and cloud
tops, which the 2015 sample does not: the UI tests draw radar from it.
"""

import argparse
import os
import re
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
T_RE = re.compile(r";t=([0-9]+)(?:\.([0-9]+))?;")


def iso_ms(ms):
    t = datetime.fromtimestamp(ms // 1000, tz=timezone.utc)
    return t.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (ms % 1000)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--test-data", default=os.path.join(HERE, ".."),
                    help="test data directory (default: the one containing this script)")
    data_dir = os.path.abspath(ap.parse_args().test_data)
    SRC = os.path.join(data_dir, "fisb", "tg30.978")
    OUT = os.path.join(data_dir, "fisb", "received-tg30.jsonl")
    lines = []
    with open(SRC) as f:
        for line in f:
            if not line.startswith("+"):
                continue
            m = T_RE.search(line)
            if not m:
                raise SystemExit("no t= in: " + line[:60])
            frac = (m.group(2) or "0")[:3].ljust(3, "0")
            ms = int(m.group(1)) * 1000 + int(frac)
            hexpart = line[1:].split(";", 1)[0]
            lines.append('{"time":"%s","type":"uat_uplink","uplink_hex":"%s"}' % (iso_ms(ms), hexpart))
    with open(OUT, "w", newline="\n") as f:
        for line in lines:
            f.write(line + "\n")
    print("%s: %d uplinks, %d bytes" % (OUT, len(lines), os.path.getsize(OUT)))


if __name__ == "__main__":
    main()
