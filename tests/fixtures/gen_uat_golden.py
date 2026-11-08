"""Helper of gen_uat_golden.sh (python 3 standard library only).

  gen_uat_golden.py OUT_TXT OUT_GZ

OUT_TXT is dump978-fa's stdout from --raw-stdout --json-stdout: raw lines
"-HEX;rssi=..;t=S;" and JSON objects whose metadata.received_at is S. Every raw
downlink line is paired with the JSON object of the same time; the gzip JSONL
has one object per downlink, in time order:
  {hex, address, qualifier, lat, lon, alt_baro, alt_geom, gs, track, vrate,
   vrate_src, callsign, category, squawk, nic, nac_p, ag, emergency}
Keys absent in the JSON are absent here. A JSON key this script does not know
is an error (so a new field of the tool is looked at, not dropped), and so is
a downlink with no JSON beyond the 5 % a demodulation miss may cost.
"""
import gzip
import json
import re
import sys

# JSON key -> golden key for the plain scalars.
SCALARS = {
    "address_qualifier": "qualifier",
    "airground_state": "ag",
    "pressure_altitude": "alt_baro",
    "geometric_altitude": "alt_geom",
    "ground_speed": "gs",
    "true_track": "track",
    "callsign": "callsign",
    "flightplan_id": "squawk",  # a CSID-0 mode status: the four octal digits
    "emitter_category": "category",
    "nic": "nic",
    "nac_p": "nac_p",
    "emergency": "emergency",
    "vv_src": "vrate_src",
}
# Keys that are read but not part of the golden.
IGNORED = {
    "north_velocity", "east_velocity", "metadata", "uplink_feedback", "utc_coupled",
    "capability_codes", "operational_modes", "mops_version", "nac_v", "nic_baro",
    "nic_supplement", "sil", "sil_supplement", "sda", "gva", "single_antenna",
    "transmit_mso", "tisb_site_id", "position", "address", "aircraft_size",
    "vertical_velocity_barometric", "vertical_velocity_geometric",
}
RAW = re.compile(r"^-([0-9a-fA-F]+);(.*)$")


def main(src, out):
    raw, objs = [], {}
    with open(src, encoding="ascii") as f:
        for line in f:
            line = line.strip()
            if line.startswith("{"):
                o = json.loads(line)
                t = o["metadata"]["received_at"]
                if t in objs:
                    sys.exit("two JSON objects at t=%r" % t)
                objs[t] = o
            elif line.startswith("-"):
                m = RAW.match(line)
                if m is None:
                    sys.exit("unparseable raw line: " + line[:80])
                t = None
                for kv in m.group(2).split(";"):
                    if kv.startswith("t="):
                        t = float(kv[2:])
                if t is None:
                    sys.exit("raw line without t=: " + line)
                raw.append((t, m.group(1).lower()))
    seen_t = set()
    rows, missed = [], []
    for t, hexs in raw:
        if t in seen_t:
            sys.exit("two raw lines at t=%r" % t)
        seen_t.add(t)
        o = objs.get(t)
        if o is None:
            missed.append(hexs)
            continue
        g = {"hex": hexs, "address": o["address"]}
        for k, v in o.items():
            if k in SCALARS:
                g[SCALARS[k]] = v
            elif k == "position":
                g["lat"], g["lon"] = v["lat"], v["lon"]
            elif k in ("vertical_velocity_barometric", "vertical_velocity_geometric"):
                g["vrate"] = v
            elif k not in IGNORED:
                sys.exit("unknown dump978-fa JSON key %r in %s" % (k, hexs))
        rows.append(g)
    with gzip.GzipFile(out, "wb", mtime=0) as z:
        for g in rows:
            z.write((json.dumps(g, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii"))
    print("golden: %d downlinks, %d paired, %d without JSON" % (len(raw), len(rows), len(missed)), file=sys.stderr)
    for h in missed:
        print("no JSON for " + h, file=sys.stderr)
    if len(missed) * 20 > len(raw) or not rows:
        sys.exit("too many downlinks without JSON")


main(sys.argv[1], sys.argv[2])
