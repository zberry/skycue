#!/usr/bin/env python3
"""Build the optional skycued LOCATIONS_FILE (id,lat,lon) from OurAirports.

Usage: python3 -I make-locations.py AIRPORTS_CSV NAVAIDS_CSV [-o OUT]
       python3 -I make-locations.py --help

Reads OurAirports' airports.csv and navaids.csv (public domain), writes one
"id,lat,lon" line per location (6 decimals, sorted by id, no header) to OUT
or stdout. An id whose positions are not all within 1 nm of the first is
ambiguous and dropped. See make-locations.md. Standard library only.

Exit status: 0 done, 1 unreadable file, 2 usage or a missing CSV column.
"""
import csv
import math
import re
import sys

ID_RE = re.compile(r"^[A-Z0-9]{2,5}$")
SAME_PLACE_NM = 1.0
# skycued reads at most this many LOCATIONS_FILE ids (kMaxOperatorRows in
# skycued/src/fisb/location_index.hpp; the test pins the two).
DAEMON_MAX_ROWS = 250000
USAGE = "usage: make-locations.py AIRPORTS_CSV NAVAIDS_CSV [-o OUT]\n"
EARTH_NM = 3440.065


class CsvError(Exception):
    pass


def distance_nm(a, b):
    lat1, lon1, lat2, lon2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = (math.sin((lat2 - lat1) / 2) ** 2
         + math.cos(lat1) * math.cos(lat2) * math.sin((lon2 - lon1) / 2) ** 2)
    return 2 * EARTH_NM * math.asin(min(1.0, math.sqrt(h)))


def read_rows(path, required):
    """Yield dict rows of a CSV; CsvError if a required column is missing."""
    with open(path, newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        names = [(n or "").strip() for n in (reader.fieldnames or [])]
        for col in required:
            if col not in names:
                raise CsvError("%s: missing column %s" % (path, col))
        reader.fieldnames = names
        for row in reader:
            yield {k: (v or "").strip() for k, v in row.items() if k}


def position(row):
    try:
        lat, lon = float(row["latitude_deg"]), float(row["longitude_deg"])
    except ValueError:
        return None
    if not (-90 <= lat <= 90 and -180 <= lon <= 180):
        return None
    return lat, lon


class Collector:
    def __init__(self):
        self.places = {}  # id -> [kind, [positions]]
        self.closed = 0
        self.invalid = 0  # rows none of whose ids was kept: a bad id or no position

    def add(self, ident, pos, kind):
        """Record ident at pos; False when dropped (a bad id or no position)."""
        ident = ident.upper()
        if pos is None or not ID_RE.match(ident):
            return False
        self.places.setdefault(ident, [kind, []])[1].append(pos)
        return True

    def airports(self, path):
        for row in read_rows(path, ("ident", "type", "latitude_deg", "longitude_deg")):
            if row["type"] == "closed":
                self.closed += 1
                continue
            pos = position(row)
            ids = [row["ident"]]
            for extra in (row.get("gps_code", ""), row.get("icao_code", "")):
                if extra and extra not in ids:
                    ids.append(extra)
            kept = [self.add(ident, pos, "airport") for ident in ids]
            if not any(kept):
                self.invalid += 1

    def navaids(self, path):
        for row in read_rows(path, ("ident", "latitude_deg", "longitude_deg")):
            if not self.add(row["ident"], position(row), "navaid"):
                self.invalid += 1

    def resolve(self):
        """Return (sorted [(id, pos, kind)], ambiguous count)."""
        out, ambiguous = [], 0
        for ident in sorted(self.places):
            kind, positions = self.places[ident]
            first = positions[0]
            if any(distance_nm(first, p) > SAME_PLACE_NM for p in positions[1:]):
                ambiguous += 1
            else:
                out.append((ident, first, kind))
        return out, ambiguous


def main(argv):
    out_path = None
    args = list(argv)
    if args in (["--help"], ["-h"]):
        sys.stdout.write(__doc__)
        return 0
    if "-o" in args:
        i = args.index("-o")
        if i + 1 >= len(args):
            sys.stderr.write("make-locations: -o needs a file name\n")
            return 2
        out_path = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2 or args[0].startswith("-") or args[1].startswith("-"):
        sys.stderr.write(USAGE)
        return 2
    c = Collector()
    try:
        c.airports(args[0])
        c.navaids(args[1])
    except CsvError as e:
        sys.stderr.write("make-locations: %s\n" % e)
        return 2
    except (OSError, UnicodeDecodeError, csv.Error) as e:
        sys.stderr.write("make-locations: cannot read input: %s\n" % e)
        return 1
    rows, ambiguous = c.resolve()
    text = "".join("%s,%.6f,%.6f\n" % (i, p[0], p[1]) for i, p, _ in rows)
    try:
        if out_path:
            with open(out_path, "w", newline="") as f:
                f.write(text)
        else:
            sys.stdout.write(text)
    except OSError as e:
        sys.stderr.write("make-locations: cannot write output: %s\n" % e)
        return 1
    airports = sum(1 for r in rows if r[2] == "airport")
    sys.stderr.write(
        "make-locations: %d locations (%d airports, %d navaids), "
        "%d ambiguous dropped, %d closed skipped, %d invalid skipped\n"
        % (len(rows), airports, len(rows) - airports, ambiguous, c.closed, c.invalid))
    if len(rows) > DAEMON_MAX_ROWS:
        sys.stderr.write(
            "make-locations: warning: %d rows is more than the %d skycued reads; "
            "the ids after the first %d (in file order) would be ignored\n"
            % (len(rows), DAEMON_MAX_ROWS, DAEMON_MAX_ROWS))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
