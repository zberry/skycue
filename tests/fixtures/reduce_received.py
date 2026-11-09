"""Reduces a received-stream JSONL recording of skycued to the shape of
modes-120s.golden.jsonl.gz (see gen_raw_golden.py), so the two compare.

  reduce_received.py [--uat] RECEIVED.jsonl OUT.jsonl.gz
  reduce_received.py --uat-golden LAT LON GOLDEN.jsonl.gz OUT.jsonl.gz

Only observations with lat and lon count; a position equal to the previous one of
the address is not a new position (as in the golden). Everything but the position
comes from the observation that carried the newest position. Besides the golden's
keys each row has `all`, every position as [lat, lon]: skycued decodes single frames
locally against its reference, so its second position is not readsb's second.

--uat is the same reduction of a UAT recording: the motion also carries alt_geom (the
geometric altitude of the newest position's frame, if it has one). --uat-golden reduces
a UAT golden (gen_uat_golden.py: one object per downlink) with the same rules, so the two
outputs compare: positions are counted in time order, one equal to the address's previous
is no new position, `vrate` goes to baro_rate or geom_rate by `vrate_src`, `callsign` to
`flight`. LAT LON is the receiver position given to skycued: the UAT decoder rejects a position
more than 300 NM from it (the reasonableness range of the 1090 decoder), so golden positions
farther away are left out and named on stderr. Standard library only.
"""
import gzip
import json
import math
import sys

IDENT = (("callsign", "flight"), ("category", "category"), ("squawk", "squawk"),
         ("nic", "nic"), ("nac_p", "nac_p"))
MOTION = (("alt_baro_ft", "alt_baro"), ("gs_kt", "gs"), ("track_deg", "track"),
          ("baro_rate_fpm", "baro_rate"), ("geom_rate_fpm", "geom_rate"),
          ("heading_mag_deg", "heading_mag_deg"), ("heading_true_deg", "heading_true_deg"),
          ("ias_kt", "ias_kt"), ("tas_kt", "tas_kt"), ("mach", "mach"), ("roll_deg", "roll_deg"),
          ("track_rate_dps", "track_rate_dps"), ("sel_alt_ft", "sel_alt_ft"),
          ("sel_alt_fms_ft", "sel_alt_fms_ft"), ("qnh_mb", "qnh_mb"),
          ("sel_heading_deg", "sel_heading_deg"), ("nav_modes", "nav_modes"))


UAT_MOTION = MOTION + (("alt_geom_ft", "alt_geom"),)


def golden_as_obs(g):
    """One golden object (gen_uat_golden.py) in the key names of an obs record."""
    o = {"type": "obs", "hex": g["address"]}
    for k in ("lat", "lon", "alt_baro", "alt_geom", "gs", "track", "category", "squawk", "nic", "nac_p"):
        if k in g:
            o[{"alt_baro": "alt_baro_ft", "alt_geom": "alt_geom_ft", "gs": "gs_kt", "track": "track_deg"}.get(k, k)] = g[k]
    if "callsign" in g:
        o["callsign"] = g["callsign"]
    if "vrate" in g:
        o["geom_rate_fpm" if g.get("vrate_src") == "geometric" else "baro_rate_fpm"] = g["vrate"]
    return o


def distance_nm(a, b):
    p1, p2 = math.radians(a[0]), math.radians(b[0])
    h = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(math.radians(b[1] - a[1]) / 2) ** 2
    return 2 * 3440.065 * math.asin(math.sqrt(h))


def main(src, out, uat=False, golden=False, ref=None):
    acc = {}
    far = set()
    motion = UAT_MOTION if uat else MOTION
    opener = gzip.open if golden else open
    with opener(src, "rt", encoding="utf-8", errors="replace") as f:
        for raw in f:
            try:
                o = json.loads(raw)
            except ValueError:
                continue
            if golden:
                o = golden_as_obs(o)
                if ref and "lat" in o and distance_nm(ref, (o["lat"], o["lon"])) > 300.0:
                    far.add(o["hex"])
                    continue
            if o.get("type") != "obs":
                continue
            hexid = o.get("hex")
            if not isinstance(hexid, str) or "lat" not in o or "lon" not in o:
                continue
            a = acc.setdefault(hexid, {"positions": 0, "pos": None, "second": None, "all": [], "ident": {}})
            if uat:  # sticky: a frame that carries the call sign or squawk sets it for the address
                for src_k, dst in IDENT:
                    if src_k in o:
                        v = o[src_k]
                        a["ident"][dst] = v.strip() if isinstance(v, str) else v
            p = (o["lat"], o["lon"])
            if p == a["pos"]:
                continue
            a["positions"] += 1
            a["all"].append([p[0], p[1]])
            a["pos"] = p
            a["t"] = o.get("time")
            a["motion"] = {dst: (sorted(o[src_k]) if dst == "nav_modes" and isinstance(o[src_k], list) else o[src_k])
                           for src_k, dst in motion if src_k in o}
            if not uat:
                a["ident"] = {}
                for src_k, dst in IDENT:
                    if src_k in o:
                        v = o[src_k]
                        a["ident"][dst] = v.strip() if isinstance(v, str) else v
            if a["positions"] == 2:
                a["second"] = {"t": a["t"], "lat": p[0], "lon": p[1]}
    rows = []
    for hexid in sorted(acc):
        a = acc[hexid]
        row = {"hex": hexid, "positions": a["positions"], "all": a["all"]}
        if a["second"]:
            row["second"] = a["second"]
        last = {"t": a["t"], "lat": a["pos"][0], "lon": a["pos"][1]}
        last.update(a["motion"])
        row["last"] = last
        row.update(a["ident"])
        rows.append(row)
    with gzip.GzipFile(out, "wb", mtime=0) as g:
        for r in rows:
            g.write((json.dumps(r, sort_keys=True, separators=(",", ":")) + "\n").encode())
    if far:
        print("left out, beyond 300 NM of the receiver: " + " ".join(sorted(far)), file=sys.stderr)
    print("excluded=%d addresses=%d with_second=%d total_positions=%d" % (
        len(far), len(rows), sum(1 for r in rows if "second" in r), sum(r["positions"] for r in rows)))


if __name__ == "__main__":
    args = sys.argv[1:]
    mode = args.pop(0) if args and args[0] in ("--uat", "--uat-golden") else None
    ref = None
    if mode == "--uat-golden":
        if len(args) != 4:
            sys.exit(__doc__)
        ref = (float(args[0]), float(args[1]))
        args = args[2:]
    if len(args) != 2:
        sys.exit(__doc__)
    main(args[0], args[1], uat=mode is not None, golden=mode == "--uat-golden", ref=ref)
