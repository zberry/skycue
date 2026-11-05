#!/usr/bin/env python3
"""Cross-check skycue-fisb --json output against fisb-decode's level-2 output.

usage: python3 -I fisb-golden-check.py FD_LEVEL2.jsonl SKYCUE_FISB.jsonl [--examples N]
       python3 -I fisb-golden-check.py --golden-only SKYCUE_FISB.jsonl

FD_LEVEL2.jsonl is what fisb-decode's level0 | level1 | level2 pipeline
prints for an uplink file (see scripts/fisb-golden.md for the invocation);
SKYCUE_FISB.jsonl is `skycue-fisb --json` over the same file. The two are not
the same kind of stream: fisb-decode level 2 prints every decoded message
(repeats included), skycue-fisb prints the events of its store (a record once
when it is new or changed, a mosaic each time it is published). So the check
is one of containment, product family by product family:

  text    every fisb_text METAR/TAF/PIREP is an FD record with the same station,
          time and text; every winds record's data line is an FD WINDS_xx_HR
          contents for that station and period, with issued = FD
          issued_time and valid = FD valid_time;
  twgo    every fisb_twgo text equals the FD record's contents (same report);
          SUA: FD has no contents, its airspace name and schedule id must be
          in our text; a NOTAM's text_end is FD's end_of_validity_time; every shape matches an FD geometry: same type, the same
          vertices (FD lon,lat; ours lat,lon) to 1e-6, same altitudes;
  blocks  every block of every published mosaic is an FD IMAGE with the same
          product (and altitude), alternate block number and all 128 bins,
          FD time no later than the mosaic's product time and at most 10 min
          before it (the store's merge window).

Before that, and alone with --golden-only (no fisb-decode output needed),
the golden is checked against the store's own rules (design §10.2, amended at
the final review):

  year    no fisb_twgo record whose report year (the key's second field) is
          not the year of its record time, the data clock (the year before
          allowed in January, the year after in December; a one-digit NOTAM
          year, products 8/16/17, matched on the last digit);
  pending no fisb_twgo record drawn as active before its start: a record
          with a start after its record time has pending true, any other
          pending false;
  stale   no AIRMET, SIGMET or CWA (products 11, 12, 15) record whose
          reference instant is more than 12 h before or more than 6 h after
          its record time: the start when it has one, else the ddhhmm
          issuance in its text header resolved to the nearest month (design
          §10.2, amended 2026-11-02).

Known representational differences (adjudicated in scripts/fisb-golden.md) are
allowed and counted, not failed. Exit status 0 when nothing else differs.
"""
import collections
import datetime
import json
import re
import sys


def die(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def load(path):
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def iso_s(t):
    """'2020-10-30T08:35:00Z' or '...00.000Z' -> '2020-10-30T08:35:00'."""
    return t[:19]


def rle_expand(rle):
    out = []
    for value, count in rle:
        out += [value] * count
    return out


def fd_alt_block(bn, scale):
    """fisb-decode msgBlock.alternateBlockNumber."""
    offset, div = {0: (0, 1), 1: (1800, 5), 2: (3600, 9)}[scale]
    row, col = divmod(bn - offset, offset + 450)
    return int(row * 1000 + col / div)


def block_geometry(bn, scale):
    """NW corner and bin size from the block number (reference §5.2, < 60 N)."""
    row, col = divmod(bn, 450)
    lat0 = (row + 1) * 4 / 60
    lon0 = col * 48 / 60
    if lon0 > 180:
        lon0 -= 360
    size = {0: 1, 1: 5, 2: 9}[scale]
    return lat0, lon0, size / 60, size * 1.5 / 60


FD_IMAGE_NAME = {63: "NEXRAD_REGIONAL", 64: "NEXRAD_CONUS", 84: "CLOUD_TOPS", 103: "LIGHTNING"}


def fd_image_name(product, altitude_code):
    if product in FD_IMAGE_NAME:
        return FD_IMAGE_NAME[product]
    base = {70: ("ICING", 2000), 71: ("ICING", 18000), 90: ("TURBULENCE", 2000), 91: ("TURBULENCE", 18000)}
    name, start = base[product]
    return "%s_%05d" % (name, start + 2000 * altitude_code)


FD_TWGO_TYPE = {8: "NOTAM", 11: "AIRMET", 12: "SIGMET", 13: "SUA", 14: "G_AIRMET", 15: "CWA"}
# fisb-decode level2/msg11_12_15.py BAD_MESSAGES: "stuck" SIGMETs it ignores on purpose.
FD_BAD_MESSAGES = [
    "WST KMKC 062057 CONVECTIVE SIGMET 99C\nFL TN AL MS LA AR TX OK AND FL AL MS LA CSTL WTRS\nFROM 20ENE MEM-20NNW "
    "VUZ-110S CEW-50SSW LSU-70NW GGG-10SSW\nFSM-20ENE MEM\nAREA TS MOV LTL. TOPS TO FL410.\n",
    "WST KMKC 170253 CONVECTIVE SIGMET 3E\nNC AND NC SC CSTL WTRS\nFROM 40S ECG-120SE ECG-200SE ILM-120SSE "
    "ILM-30WSW ILM-40S ECG\nAREA EMBD TS MOV FROM 17015KT. TOPS TO FL430.\n",
]
FD_GEOM = {"POLYGON": "polygon", "POLYLINE": "polyline", "POINT": "point", "CIRCLE": "circle"}


NOTAM_HEADER = re.compile(r"^NOTAM-[A-Z-]+ \S+ \S+ $")


def notam_body_matches(ours, fd):
    def trim(s):
        return "\n".join(line.rstrip() for line in s.split("\n")).strip()
    a, b = trim(ours), trim(fd)
    return bool(b) and a.endswith(b) and bool(NOTAM_HEADER.match(a[: len(a) - len(b)]))


def plausible_year(pid, year, data_time):
    """The store's report-year rule (fisb_store.cpp plausibleYear)."""
    y, m = int(data_time[0:4]), int(data_time[5:7])
    cands = [y] + ([y - 1] if m == 1 else []) + ([y + 1] if m == 12 else [])
    return any(year == c % 100 or (pid in (8, 16, 17) and year < 10 and year == c % 10) for c in cands)


HEADER_DDHHMM = re.compile(r"(?<![0-9])([0-9]{2})([0-9]{2})([0-9]{2})(?![0-9])")


def parse_time(t):
    return datetime.datetime.strptime(iso_s(t), "%Y-%m-%dT%H:%M:%S")


def header_issued(text, now):
    """The store's headerIssued: the first six-digit group on the text's first
    line as ddhhmm, in the month (of now's -1/0/+1) nearest now."""
    m = HEADER_DDHHMM.search(text.split("\n")[0])
    if not m:
        return None
    dd, hh, mi = (int(x) for x in m.groups())
    if hh > 24 or mi > 59 or (hh == 24 and mi) or not 1 <= dd <= 31:
        return None
    best = None
    for k in (-1, 0, 1):
        y, mo = now.year, now.month + k
        if mo < 1:
            y, mo = y - 1, 12
        elif mo > 12:
            y, mo = y + 1, 1
        try:
            t = datetime.datetime(y, mo, dd) + datetime.timedelta(hours=hh, minutes=mi)
        except ValueError:
            continue
        if best is None or abs(t - now) < abs(best - now):
            best = t
    return best


def self_check(ours, ok):
    """The golden against the store's own rules (no fisb-decode needed)."""
    for o in ours:
        if o["type"] != "fisb_twgo":
            continue
        pid, year = (int(x) for x in o["key"].split("-")[0:2])
        if not plausible_year(pid, year, o["time"]):
            die("TWGO %s: report year %d is not the data clock's year (record time %s)" % (o["key"], year, o["time"]))
        ok["self: TWGO year = data year"] += 1
        if "pending" not in o:
            die("TWGO %s: no pending field" % o["key"])
        want = "start" in o and o["start"] > o["time"]
        if o["pending"] != want:
            die("TWGO %s: start %s at %s but pending %s (a not-yet-in-force record drawn as active, or the reverse)"
                % (o["key"], o.get("start"), o["time"], o["pending"]))
        ok["self: TWGO pending = start ahead"] += 1
        if o["pending"]:
            ok["self: TWGO pending records"] += 1
        if pid in (11, 12, 15):
            now = parse_time(o["time"])
            ref = parse_time(o["start"]) if "start" in o else header_issued(o.get("text") or "", now)
            if ref is not None:
                if ref < now - datetime.timedelta(hours=12) or ref > now + datetime.timedelta(hours=6):
                    die("TWGO %s: reference instant %s more than 12 h before or 6 h after its time %s "
                        "(a stale-on-arrival report published)" % (o["key"], ref.isoformat(), o["time"]))
                ok["self: TWGO 11/12/15 ref in window"] += 1


def main():
    args = sys.argv[1:]
    if args and args[0] == "--golden-only":
        if len(args) != 2:
            print(__doc__)
            sys.exit(2)
        ok = collections.Counter()
        self_check(load(args[1]), ok)
        for k, v in sorted(ok.items()):
            print("  %-36s %d" % (k, v))
        print("OK")
        return
    examples = 0
    if "--examples" in args:
        i = args.index("--examples")
        examples = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        sys.exit(2)
    fd = load(args[0])
    ours = load(args[1])
    ok = collections.Counter()
    allowed = collections.Counter()
    shown = collections.Counter()
    self_check(ours, ok)
    data_time = next((o["time"] for o in ours if "time" in o), "")

    def show(kind, text):
        if shown[kind] < examples:
            shown[kind] += 1
            print("  [%s] %s" % (kind, text))

    # ---- text ----
    fd_text = set()
    fd_winds = set()
    for d in fd:
        t = d["type"]
        if t == "METAR":
            fd_text.add(("metar", d["location"], iso_s(d["observation_time"]), d["contents"]))
        elif t == "TAF":
            fd_text.add(("taf", d["location"], iso_s(d["issued_time"]), d["contents"]))
        elif t == "PIREP":
            fd_text.add(("pirep", d["station"], iso_s(d["report_time"]), d["contents"]))
        elif t.startswith("WINDS_"):
            period = int(t.split("_")[1])
            fd_winds.add((d["location"], period, iso_s(d["issued_time"]), iso_s(d["valid_time"]), d["contents"]))
    for o in ours:
        if o["type"] != "fisb_text":
            continue
        kind = o["kind"]
        if kind == "winds":
            data = o["text"].split("\n", 1)[1] if "\n" in o["text"] else ""
            if "valid" not in o:
                die("winds %s without a valid time" % o["key"])
            key = (o["station"], o["period_h"], iso_s(o["issued"]), iso_s(o["valid"]), data)
            if key not in fd_winds:
                die("winds not in fisb-decode output (station, period, issued, valid, data): %r" % (key,))
            ok["winds"] += 1
            show("winds", "%s %sh issued %s valid %s: data line equal" % key[:4])
            continue
        key = (kind, o["station"], iso_s(o["issued"]), o["text"])
        if key not in fd_text:
            other = [k for k in fd_text if k[0] == kind and k[2:] == key[2:]]
            if kind == "pirep" and other:
                # FD takes the station from the report's own location field
                # ("MEM UA"); ours is the first token after the type ("KMEM").
                allowed["PIREP station token: ours %s, FD %s" % (o["station"], other[0][1])] += 1
                continue
            die("%s not in fisb-decode output: %r" % (kind, key))
        ok[kind] += 1
        show(kind, "%s %s %s: text equal" % key[:3])

    # ---- TWGO ----
    fd_twgo = collections.defaultdict(list)
    for d in fd:
        if d["type"] in FD_TWGO_TYPE.values():
            fd_twgo[(d["type"], d["unique_name"])].append(d)

    def fd_names(o):
        year, report = o["key"].split("-")[1:3]
        base = "%s-%s" % (year, report)
        loc = o.get("location", "")
        return [base + "-" + loc, base] if loc else [base]

    def shape_matches(s, g):
        if FD_GEOM.get(g["type"]) != (s["geom"] if s["geom"] != "prism" else "circle"):
            return None
        coords = g["coordinates"]
        if g["type"] == "POINT":
            coords = [coords]
        elif g["type"] == "CIRCLE":
            coords = [[g["lon"], g["lat"]]] if "lon" in g else coords
        pts = [[round(lon, 6), round(lat, 6)] for lat, lon in s["points"]]
        fpts = [[round(c[0], 6), round(c[1], 6)] for c in coords]
        if pts != fpts:
            return None
        alts = g.get("altitudes")
        if alts and "alt_high_ft" in s:
            top, bottom = alts[0], alts[2]
            if s["alt_high_ft"] != top:
                return None
            if s["alt_low_ft"] == bottom:
                return "exact"
            if bottom == 0 and s["alt_low_ft"] == s["alt_high_ft"]:
                return "single-altitude"  # FD fills the missing bottom with 0
            return None
        return "exact"

    for o in ours:
        if o["type"] != "fisb_twgo":
            continue
        ftype = FD_TWGO_TYPE[o["product"]]
        cands = []
        for name in fd_names(o):
            cands += fd_twgo.get((ftype, name), [])
        if not cands and o.get("text", "") + "\n" in FD_BAD_MESSAGES:
            allowed["SIGMET on FD's hard-coded BAD_MESSAGES list (msg11_12_15.py): FD drops it"] += 1
            continue
        if not cands:
            die("TWGO %s: no %s %s in fisb-decode output" % (o["key"], ftype, fd_names(o)))
        if "text" in o:
            if ftype == "SUA":
                if not any(c["airspace_name"] in o["text"] and c["schedule_id"] in o["text"] for c in cands):
                    die("SUA %s: FD airspace/schedule not in our text" % o["key"])
                ok["sua text"] += 1
            elif any(c.get("contents") == o["text"] for c in cands):
                ok["twgo text"] += 1
                show("twgo text", "%s: text equal (%d chars)" % (o["key"], len(o["text"])))
            elif ftype == "NOTAM" and any(notam_body_matches(o["text"], c.get("contents", "")) for c in cands):
                # FD drops the "NOTAM-D KEYE.10/028 010100Z " header and the
                # spaces before each line break; ours is the text as sent.
                allowed["NOTAM text: FD strips the NOTAM-x header and line-end spaces"] += 1
                show("notam text", "%s: text equal after FD's header strip (%d chars)" % (o["key"], len(o["text"])))
            else:
                die("TWGO %s: text differs from every FD %s %s" % (o["key"], ftype, fd_names(o)))
        if ftype == "NOTAM" and "text" in o:
            fd_end = set(c.get("end_of_validity_time") for c in cands) - {None}
            fd_end.discard("2038-01-01T00:00:00Z")  # FD's PERM stand-in; ours has no text_end then
            if o.get("text_end") and {iso_s(o["text_end"])} != {iso_s(t) for t in fd_end}:
                die("NOTAM %s: text_end %s, FD end_of_validity_time %s" % (o["key"], o["text_end"], fd_end))
            if not o.get("text_end") and fd_end:
                die("NOTAM %s: no text_end, FD end_of_validity_time %s" % (o["key"], fd_end))
            if o.get("text_end"):
                ok["NOTAM text_end"] += 1
        for s in o["shapes"]:
            how = None
            for c in cands:
                for g in c.get("geometry", []):
                    how = shape_matches(s, g)
                    if how:
                        break
                if how:
                    break
            if not how:
                die("TWGO %s: %s shape with %d points matches no FD geometry" % (o["key"], s["geom"], len(s["points"])))
            if how == "exact":
                ok["shapes"] += 1
            else:
                allowed["shape altitudes: single altitude (FD bottom 0, ours low = high)"] += 1
            show("shape", "%s: %s, %d points, alt %s-%s %s" % (o["key"], s["geom"], len(s["points"]),
                 s.get("alt_low_ft"), s.get("alt_high_ft"), how))

    # ---- global blocks ----
    fd_blocks = collections.defaultdict(list)
    for d in fd:
        if d["type"] == "IMAGE":
            t = d.get("observation_time") or d.get("valid_time")
            fd_blocks[(d["unique_name"], d["alt_bn"])].append((iso_s(t), [ord(ch) for ch in d["bins"]]))
    from datetime import datetime, timedelta

    def ts(t):
        return datetime.strptime(t, "%Y-%m-%dT%H:%M:%S")

    for o in ours:
        if o["type"] not in ("fisb_radar", "fisb_raster"):
            continue
        name = fd_image_name(o["product"], o.get("altitude_code"))
        pt = ts(iso_s(o["product_time"]))
        for b in o["blocks"]:
            lat0, lon0, dlat, dlon = block_geometry(b["block"], o["scale"])
            if max(abs(lat0 - b["lat0"]), abs(lon0 - b["lon0"]), abs(dlat - b["dlat"]), abs(dlon - b["dlon"])) > 1e-6:
                die("%s block %d: geometry %r" % (o["key"], b["block"], b))
            bins = rle_expand(b["rle"])
            if len(bins) != 128:
                die("%s block %d: %d bins" % (o["key"], b["block"], len(bins)))
            alt = fd_alt_block(b["block"], o["scale"])
            match = None
            for t, fbins in fd_blocks.get((name, alt), []):
                if not (pt - timedelta(minutes=10) <= ts(t) <= pt):
                    continue
                if fbins == bins:
                    match = "exact"
                    break
                if o["product"] == 64 and set(fbins) == {0} and set(bins) == {1}:
                    match = "conus-empty"
            if not match:
                die("%s block %d (FD %s %d) at %s: no FD IMAGE with the same bins" % (o["key"], b["block"], name, alt,
                                                                                 o["product_time"]))
            if match == "exact":
                ok["blocks"] += 1
            else:
                allowed["CONUS empty block: ours level 1 (ruling 10.6), FD 0"] += 1
        ok["mosaic publishes"] += 1
        show("mosaic", "%s at %s: %d blocks, all bins equal" % (o["key"], o["product_time"], len(o["blocks"])))

    # ---- the other way: every distinct FD text and TWGO report is in ours ----
    ours_text = set((o["kind"], o["station"], iso_s(o["issued"])) for o in ours if o["type"] == "fisb_text")
    for kind, fdtype, tkey in (("metar", "METAR", "observation_time"), ("taf", "TAF", "issued_time")):
        for d in fd:
            if d["type"] == fdtype and (kind, d["location"], iso_s(d[tkey])) not in ours_text:
                die("FD %s %s %s never published by skycue-fisb" % (fdtype, d["location"], d[tkey]))
        ok["FD %s covered" % fdtype] = len(set((d["location"], d[tkey]) for d in fd if d["type"] == fdtype))
    ours_twgo = set()
    for o in ours:
        if o["type"] == "fisb_twgo":
            for name in fd_names(o):
                ours_twgo.add((FD_TWGO_TYPE[o["product"]], name))
    for key, recs in sorted(fd_twgo.items()):
        fd_pid = 8 if key[0] == "NOTAM" else next(p for p, t in FD_TWGO_TYPE.items() if t == key[0])
        fd_year = int(key[1].split("-")[0])
        if key in ours_twgo:
            ok["FD TWGO reports covered"] += 1
        elif data_time and not plausible_year(fd_pid, fd_year, data_time):
            # A report of another year (a stuck one): the store rejects it
            # (store.wrong_year, design §10.2 amended at the final review).
            allowed["FD TWGO report of another year than the data: rejected by the store"] += 1
        elif all(r.get("renew-only") == "1" for r in recs):
            # A TFR keep-alive for a report whose text never came: the store
            # creates nothing for it (design §10.2), FD prints the renewal.
            allowed["FD TFR renew-only for a report never sent in full: no record"] += 1
        else:
            die("FD %s %s never published by skycue-fisb" % key)

    print("matched:")
    for k, v in sorted(ok.items()):
        print("  %-20s %d" % (k, v))
    print("allowed differences:")
    for k, v in sorted(allowed.items()):
        print("  %-70s %d" % (k, v))
    print("OK")


if __name__ == "__main__":
    main()
