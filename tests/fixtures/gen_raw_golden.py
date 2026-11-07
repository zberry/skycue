"""Helper of gen_raw_golden.sh (python 3 standard library only).

  collect PORT OUT      connect to readsb's JSON port, append every line to OUT
                        until the connection closes
  reduce LINES OUT_GZ   one object per address, gzip JSONL sorted by hex:
    {hex, positions, second{t,lat,lon}, last{t,lat,lon,alt_baro,gs,track,
     baro_rate,geom_rate}, flight, category, squawk, nic, nac_p}
"""
import gzip
import json
import socket
import sys


def collect(port, out):
    with socket.create_connection(("127.0.0.1", int(port)), timeout=30) as s, open(out, "wb") as f:
        s.settimeout(None)
        while True:
            data = s.recv(65536)
            if not data:
                break
            f.write(data)
            f.flush()


# Identity fields, read from the line of the last position.
IDENT = (("flight", "flight"), ("category", "category"), ("squawk", "squawk"),
         ("nic", "nic"), ("nac_p", "nac_p"))
# Fields of the "last" object (absent when that line has none).
MOTION = (("alt_baro", "alt_baro"), ("gs", "gs"), ("track", "track"),
          ("baro_rate", "baro_rate"), ("geom_rate", "geom_rate"))


def reduce(lines, out):
    acc = {}
    with open(lines, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            try:
                o = json.loads(raw)
            except ValueError:
                continue
            hexid = o.get("hex")
            if not isinstance(hexid, str) or hexid.startswith("~"):
                continue
            a = acc.setdefault(hexid, {"hex": hexid, "positions": 0, "second": None, "pos": None,
                                       "motion": {}, "ident": {}})
            if "lat" in o and "lon" in o:
                p = (o["lat"], o["lon"])
                if p != a["pos"]:
                    a["positions"] += 1
                    a["pos"] = p
                    a["t"] = o.get("now")
                    # Everything below is the line that carried the newest position, so a
                    # comparison at our own latest position sees the same moment.
                    a["motion"] = {dst: o[src] for src, dst in MOTION if src in o}
                    a["ident"] = {}
                    for src, dst in IDENT:
                        if src in o:
                            v = o[src]
                            a["ident"][dst] = v.strip() if isinstance(v, str) else v
                    if a["positions"] == 2:
                        a["second"] = {"t": a["t"], "lat": p[0], "lon": p[1]}
    rows = []
    for hexid in sorted(acc):
        a = acc[hexid]
        row = {"hex": hexid, "positions": a["positions"]}
        if a["second"]:
            row["second"] = a["second"]
        if a["pos"]:
            last = {"t": a["t"], "lat": a["pos"][0], "lon": a["pos"][1]}
            last.update(a["motion"])
            row["last"] = last
        row.update(a["ident"])
        rows.append(row)
    with gzip.GzipFile(out, "wb", mtime=0) as g:
        for r in rows:
            g.write((json.dumps(r, sort_keys=True, separators=(",", ":")) + "\n").encode())
    print("addresses=%d positioned=%d with_second=%d total_positions=%d" % (
        len(rows), sum(1 for r in rows if "last" in r), sum(1 for r in rows if "second" in r),
        sum(r["positions"] for r in rows)))


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "collect":
        collect(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "reduce":
        reduce(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
