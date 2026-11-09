#!/usr/bin/env python3
"""Generate readsb-sample.jsonl: synthetic lines in the format of readsb's JSON
output port (--net-json-port), one aircraft object per line with a leading
"now" (seconds since the epoch), keys in readsb's order.

Regenerate the committed sample with (from the repository root):

    python3 -I tests/data/gen_readsb_jsonl.py > tests/data/readsb-sample.jsonl

Standard library only and deterministic (random.seed(1)), so rerunning gives a
byte-identical file.

Receiver at 38.9000, -77.0000. Each aircraft reports at 1 Hz for t = 0..120 s
(121 lines) unless noted; every aircraft has its own sub-second phase, so the
lines of different aircraft interleave in "now" order. Aircraft (range and
bearing are from the receiver at t = 0):

  1090 ES airborne, straight lines (type adsb_icao):
   a4f2c1 UAL1423  B738  FL360, 460 kt, track 270,  60 nm at 090 (inbound), has r/t and ias/tas/mach/nav_*
   a8c3e5 DAL872   A321  FL280, 420 kt, track 000, 145 nm at 000: crosses the 150 nm ring outbound near t = 44 s
   ab12f0 AAL2291        12000 ft climbing 1800 fpm, 300 kt, track 135, 20 nm at 315
   a0d4b7 SWA3310        4500 ft descending 1500 fpm, 250 kt, track 190, 18 nm at 010
   a5b06d N512RG   C172  3000 ft, 115 kt, track 090, 40 nm at 180: about 0.4 deg elevation
                         (atan(914 m / 74 km) - 74 km / (2 * 6371 km)), below the 2 deg mask
   a6e9d2 JBU1187        FL380, 480 kt, track 045, 155 nm at 225: crosses the 150 nm ring inbound near t = 39 s
  978 UAT, as readsb ingests it (type adsr_icao):
   a3b5f8 N3157K         5500 ft, 140 kt, track 300, 25 nm at 120
   a91c42 N72HX          2500 ft, 95 kt, track 060, 12 nm at 250 (category A7, rotorcraft)
  TIS-B (type tisb_other, non-ICAO address so the hex carries readsb's "~"):
   ~2c4a1f               1800 ft, 105 kt, track 210, 8 nm at 030; no callsign or squawk
  On the ground (alt_baro "ground", no alt_geom):
   a7d3e6 UAL455         taxiing at 14 kt, track 180, about 3.5 nm south-southwest (Reagan National)
  No position for the first 30 s (lines without lat/lon/nic/rc/seen_pos), positions after:
   a2f7b9 FDX1520        FL240, 410 kt, track 090, 50 nm at 260
  Stops transmitting at t = 60 s (lines for t = 0..59 only), so it times out:
   ac5e81 RPA4521        FL180, 380 kt, track 330, 70 nm at 150

Then three deliberately odd lines, after t = 120 s:
  1. a4f2c1 with an unknown extra key ("x_unknown") that parsers must ignore;
  2. a0d4b7 (until now airborne, with alt_geom) reporting alt_baro "ground" and no alt_geom;
  3. a8c3e5 with no lat/lon (position aged out) and readsb's nested "lastPosition" object instead.
"""

import json
import math
import random
import sys

RX_LAT = 38.9000
RX_LON = -77.0000
BASE_NOW = 1793545200.0  # 2026-11-01T15:00:00Z
DURATION_S = 120
EARTH_NM = 3440.065

ADSB_TAIL = [("version", 2), ("nic_baro", 1), ("nac_p", 9), ("nac_v", 1), ("sil", 3),
             ("sil_type", "perhour"), ("gva", 2), ("sda", 2)]

# hex, type, flight, r, t, alt_ft, rate_fpm, gs_kt, track_deg, start_nm, start_brg, extras
AIRCRAFT = [
    dict(hex="a4f2c1", type="adsb_icao", flight="UAL1423", r="N37502", t="B738", alt=36000, rate=0,
         gs=460, track=270, nm=60, brg=90, squawk="3341", cat="A3", rssi=-9.5, extras=True),
    dict(hex="a8c3e5", type="adsb_icao", flight="DAL872", r="N391DN", t="A321", alt=28000, rate=0,
         gs=420, track=0, nm=145, brg=0, squawk="5124", cat="A3", rssi=-19.0),
    dict(hex="ab12f0", type="adsb_icao", flight="AAL2291", alt=12000, rate=1800,
         gs=300, track=135, nm=20, brg=315, squawk="2715", cat="A3", rssi=-8.0),
    dict(hex="a0d4b7", type="adsb_icao", flight="SWA3310", alt=4500, rate=-1500,
         gs=250, track=190, nm=18, brg=10, squawk="4402", cat="A3", rssi=-7.0),
    dict(hex="a5b06d", type="adsb_icao", flight="N512RG", t="C172", alt=3000, rate=0,
         gs=115, track=90, nm=40, brg=180, squawk="1200", cat="A1", rssi=-17.5),
    dict(hex="a6e9d2", type="adsb_icao", flight="JBU1187", alt=38000, rate=0,
         gs=480, track=45, nm=155, brg=225, squawk="6631", cat="A3", rssi=-19.5),
    dict(hex="a3b5f8", type="adsr_icao", flight="N3157K", alt=5500, rate=0,
         gs=140, track=300, nm=25, brg=120, squawk="1200", cat="A1", rssi=-14.0),
    dict(hex="a91c42", type="adsr_icao", flight="N72HX", alt=2500, rate=0,
         gs=95, track=60, nm=12, brg=250, squawk="1200", cat="A7", rssi=-12.0),
    dict(hex="~2c4a1f", type="tisb_other", alt=1800, rate=0,
         gs=105, track=210, nm=8, brg=30, rssi=-11.0, tisb=True),
    dict(hex="a7d3e6", type="adsb_icao", flight="UAL455", alt=None, rate=0,
         gs=14, track=180, nm=3.5, brg=200, squawk="1756", cat="A3", rssi=-6.0, ground=True),
    dict(hex="a2f7b9", type="adsb_icao", flight="FDX1520", alt=24000, rate=0,
         gs=410, track=90, nm=50, brg=260, squawk="3062", cat="A5", rssi=-15.5, nopos_until=30),
    dict(hex="ac5e81", type="adsb_icao", flight="RPA4521", alt=18000, rate=0,
         gs=380, track=330, nm=70, brg=150, squawk="4711", cat="A3", rssi=-16.5, stop_at=60),
]


def destination(lat, lon, bearing_deg, dist_nm):
    """Great-circle destination point."""
    d = dist_nm / EARTH_NM
    b = math.radians(bearing_deg)
    p1 = math.radians(lat)
    l1 = math.radians(lon)
    p2 = math.asin(math.sin(p1) * math.cos(d) + math.cos(p1) * math.sin(d) * math.cos(b))
    l2 = l1 + math.atan2(math.sin(b) * math.sin(d) * math.cos(p1),
                         math.cos(d) - math.sin(p1) * math.sin(p2))
    return math.degrees(p2), math.degrees(l2)


def fmt(value):
    if isinstance(value, str):
        return json.dumps(value)
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, list):
        return "[" + ",".join(fmt(v) for v in value) + "]"
    if isinstance(value, tuple):  # (number, decimals)
        return "%.*f" % (value[1], value[0])
    return str(value)


def line(fields):
    return "{" + ",".join('"%s":%s' % (k, fmt(v)) for k, v in fields) + "}"


def round_to(x, step):
    return int(round(x / step)) * step


def make_line(ac, t, now, state, with_position=True):
    """One readsb line for aircraft `ac` at elapsed time t (now = epoch seconds)."""
    seen_pos = round(random.uniform(0.0, 0.9), 1)
    seen = round(random.uniform(0.0, seen_pos), 1)
    pos_t = t - seen_pos
    start = state["start"]
    lat, lon = destination(start[0], start[1], ac["track"], ac["gs"] * max(pos_t, 0.0) / 3600.0)
    gs = ac["gs"] + random.uniform(-1.5, 1.5)
    track = (ac["track"] + random.uniform(-0.4, 0.4)) % 360.0
    state["messages"] += random.randint(2, 7)
    rssi = max(-20.0, min(-5.0, ac["rssi"] + random.uniform(-1.2, 1.2)))

    f = [("now", (now, 3)), ("hex", ac["hex"]), ("type", ac["type"])]
    if "flight" in ac:
        f.append(("flight", "%-8s" % ac["flight"]))
    if "r" in ac:
        f.append(("r", ac["r"]))
    if "t" in ac:
        f.append(("t", ac["t"]))
    if ac.get("ground"):
        f.append(("alt_baro", "ground"))
    else:
        alt = ac["alt"] + ac["rate"] * t / 60.0
        f.append(("alt_baro", round_to(alt, 25)))
        if not ac.get("tisb"):
            f.append(("alt_geom", round_to(alt + 75 + alt * 0.0125, 25)))
    f.append(("gs", (gs, 1)))
    if ac.get("extras"):
        f += [("ias", 268), ("tas", 474), ("mach", (0.808, 3))]
    f.append(("track", (track, 2)))
    if not ac.get("ground"):
        rate = round_to(ac["rate"] + random.uniform(-64, 64), 64)
        f.append(("baro_rate", rate))
        if not ac.get("tisb"):
            f.append(("geom_rate", round_to(ac["rate"] + random.uniform(-64, 64), 32)))
    if "squawk" in ac:
        f += [("squawk", ac["squawk"]), ("emergency", "none"), ("category", ac["cat"])]
    if ac.get("extras"):
        f += [("nav_qnh", (1013.6, 1)), ("nav_altitude_mcp", 36000), ("nav_heading", (270.0, 1))]
    if with_position:
        f += [("lat", (lat, 6)), ("lon", (lon, 6)), ("nic", 8), ("rc", 186), ("seen_pos", (seen_pos, 1))]
    if ac.get("tisb"):
        f += [("nac_p", 9), ("nac_v", 1), ("sil", 3), ("sil_type", "perhour"),
              ("mlat", []), ("tisb", ["altitude", "gs", "track", "lat", "lon"])]
    else:
        f += ADSB_TAIL
        f += [("alert", 0), ("spi", 0), ("mlat", []), ("tisb", [])]
    f += [("messages", state["messages"]), ("seen", (seen, 1)), ("rssi", (rssi, 1))]
    return f, (lat, lon)


def main():
    random.seed(1)
    states = {}
    for ac in AIRCRAFT:
        states[ac["hex"]] = dict(start=destination(RX_LAT, RX_LON, ac["brg"], ac["nm"]),
                                 messages=random.randint(200, 5000),
                                 phase=round(random.uniform(0.0, 0.95), 2))
    out = []  # (now, text)
    last = {}
    for t in range(DURATION_S + 1):
        for ac in AIRCRAFT:
            if t >= ac.get("stop_at", DURATION_S + 1):
                continue
            st = states[ac["hex"]]
            now = BASE_NOW + t + st["phase"] + round(random.uniform(0.0, 0.04), 3)
            fields, _ = make_line(ac, t, now, st, with_position=t >= ac.get("nopos_until", 0))
            out.append((now, line(fields)))
            last[ac["hex"]] = (t, fields)
    out.sort(key=lambda x: x[0])
    lines = [text for _, text in out]

    odd_now = BASE_NOW + DURATION_S + 1.5

    # 1. Unknown extra key.
    fields = [(k, v) for k, v in last["a4f2c1"][1]]
    fields[0] = ("now", (odd_now, 3))
    fields.insert(len(fields) - 3, ("x_unknown", "ignored by parsers"))
    lines.append(line(fields))

    # 2. alt_baro "ground" and no alt_geom, from an aircraft that had both.
    fields = [(k, v) for k, v in last["a0d4b7"][1] if k not in ("alt_geom", "baro_rate", "geom_rate")]
    fields[0] = ("now", (odd_now + 0.1, 3))
    fields = [(k, "ground") if k == "alt_baro" else (k, v) for k, v in fields]
    lines.append(line(fields))

    # 3. No lat/lon; readsb's lastPosition object instead.
    src = dict(last["a8c3e5"][1])
    fields = [(k, v) for k, v in last["a8c3e5"][1]
              if k not in ("lat", "lon", "nic", "rc", "seen_pos")]
    fields[0] = ("now", (odd_now + 0.2, 3))
    last_pos = '{"lat":%s,"lon":%s,"nic":8,"rc":186,"seen_pos":61.4}' % (fmt(src["lat"]), fmt(src["lon"]))
    text = line(fields)
    text = text[:-1] + ',"lastPosition":' + last_pos + "}"
    lines.append(text)

    sys.stdout.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
