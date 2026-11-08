"""Reduces a received-stream JSONL recording of skycued to the shape of
modes-120s.golden.jsonl.gz (see gen_raw_golden.py), so the two compare.

  reduce_received.py RECEIVED.jsonl OUT.jsonl.gz

Only observations with lat and lon count; a position equal to the previous one of
the address is not a new position (as in the golden). Everything but the position
comes from the observation that carried the newest position. Besides the golden's
keys each row has `all`, every position as [lat, lon]: skycued decodes single frames
locally against its reference, so its second position is not readsb's second. Standard library only.
"""
import gzip
import json
import sys

IDENT = (("callsign", "flight"), ("category", "category"), ("squawk", "squawk"),
         ("nic", "nic"), ("nac_p", "nac_p"))
MOTION = (("alt_baro_ft", "alt_baro"), ("gs_kt", "gs"), ("track_deg", "track"),
          ("baro_rate_fpm", "baro_rate"), ("geom_rate_fpm", "geom_rate"))


def main(src, out):
    acc = {}
    with open(src, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            try:
                o = json.loads(raw)
            except ValueError:
                continue
            if o.get("type") != "obs":
                continue
            hexid = o.get("hex")
            if not isinstance(hexid, str) or "lat" not in o or "lon" not in o:
                continue
            a = acc.setdefault(hexid, {"positions": 0, "pos": None, "second": None, "all": []})
            p = (o["lat"], o["lon"])
            if p == a["pos"]:
                continue
            a["positions"] += 1
            a["all"].append([p[0], p[1]])
            a["pos"] = p
            a["t"] = o.get("time")
            a["motion"] = {dst: o[src_k] for src_k, dst in MOTION if src_k in o}
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
    print("addresses=%d with_second=%d total_positions=%d" % (
        len(rows), sum(1 for r in rows if "second" in r), sum(r["positions"] for r in rows)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
