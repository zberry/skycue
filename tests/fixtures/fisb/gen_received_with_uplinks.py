#!/usr/bin/env python3
"""Generate received-with-uplinks.jsonl: a received-stream recording with FIS-B.

Deterministic; no arguments; run from anywhere:

    python3 tests/fixtures/fisb/gen_received_with_uplinks.py

Inputs (both committed):
  ../raw/uat-sample.txt.gz   the 2015 dump978 sample; its 704 uplink ('+') lines
  ../readsb-sample.jsonl     synthetic readsb JSON-port lines

Output (next to this script): received-with-uplinks.jsonl, one record per line,
in time order:
  - every uplink of the sample as a received-stream "uat_uplink" record, exactly
    as skycued's Recorder writes it ({"time","type","uplink_hex"}), at
    2015-01-24T04:12:00.000Z + i seconds for the i-th uplink (1 Hz). The sample
    has no receive times; its NEXRAD product time is 04:10 and its METARs are
    from about 03:53 on 2015-01-24 (docs/fisb-reference.md section 7, 9), so
    this clock keeps every product inside its lifetime for the whole recording.
  - the first OBS_SECONDS seconds of readsb-sample.jsonl, each line unchanged
    except its leading "now", shifted onto the same 2015 clock (the first line
    lands at the first uplink's time). skycued's replay reads such readsb
    JSON-port lines alongside received-stream records.
"""

import gzip
import os
import re
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLE = os.path.join(HERE, "..", "raw", "uat-sample.txt.gz")
READSB = os.path.join(HERE, "..", "readsb-sample.jsonl")
OUT = os.path.join(HERE, "received-with-uplinks.jsonl")

T0 = datetime(2015, 1, 24, 4, 12, 0, tzinfo=timezone.utc)
OBS_SECONDS = 30.0
NOW_RE = re.compile(r'^\{"now":([0-9]+(?:\.[0-9]+)?),')


def iso_ms(t):
    return t.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (t.microsecond // 1000)


def main():
    records = []  # (epoch_ms, seq, line)
    t0_ms = int(T0.timestamp() * 1000)

    with gzip.open(SAMPLE, "rt") as f:
        uplinks = [l.strip() for l in f if l.startswith("+")]
    for i, line in enumerate(uplinks):
        hexpart = line[1:].split(";", 1)[0]
        ms = t0_ms + i * 1000
        t = T0 + timedelta(milliseconds=ms - t0_ms)
        records.append((ms, len(records),
                        '{"time":"%s","type":"uat_uplink","uplink_hex":"%s"}' % (iso_ms(t), hexpart)))

    first_now = None
    with open(READSB) as f:
        for line in f:
            line = line.rstrip("\n")
            m = NOW_RE.match(line)
            if not m:
                continue
            now_ms = round(float(m.group(1)) * 1000)
            if first_now is None:
                first_now = now_ms
            if now_ms - first_now >= OBS_SECONDS * 1000:
                break
            ms = t0_ms + (now_ms - first_now)
            new_now = "%d.%03d" % (ms // 1000, ms % 1000)
            records.append((ms, len(records), '{"now":' + new_now + "," + line[m.end():]))

    records.sort()
    with open(OUT, "w", newline="\n") as f:
        for _, _, line in records:
            f.write(line + "\n")
    print("%s: %d uplinks, %d readsb lines, %d bytes"
          % (OUT, len(uplinks), len(records) - len(uplinks), os.path.getsize(OUT)))


if __name__ == "__main__":
    main()
