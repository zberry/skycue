#!/usr/bin/env bash
# skycue-stats on the readsb JSON fixture (text, --json, .gz, --config, no
# receiver, byte-identical reruns), and on the received and sent recordings a
# real skycued writes while replaying that fixture.
# usage: test_stats_cli.sh SKYCUED SKYCUE_STATS FIXTURE_DIR
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib/test_common.sh"
skycued=$1 stats=$2 fixtures=$3
sample="$fixtures/readsb-sample.jsonl"
test_init stats_cli
fail_context() {
  [ -s "$tmp/skycued.log" ] && { echo "--- skycued log:"; tail -n 20 "$tmp/skycued.log"; }
}
for f in "$skycued" "$stats" "$sample"; do [ -e "$f" ] || fail "$f missing"; done

# section FILE TITLE: the lines of one "== TITLE..." section, header excluded.
section() { awk -v t="== $2" 'index($0, t) == 1 { on = 1; next } /^== / { on = 0 } on' "$1"; }

# ---- (a) text on the fixture -------------------------------------------------
"$stats" --lat 38.9 --lon -77.0 "$sample" >"$tmp/a.out" 2>"$tmp/a.err" || { cat "$tmp/a.err"; fail "(a) exit $?"; }
grep -qx 'Aircraft: 12' "$tmp/a.out" || { cat "$tmp/a.out"; fail "(a) no 'Aircraft: 12'"; }
grep -qE '^  [^ ]*readsb-sample\.jsonl +readsb-raw +1394 records$' "$tmp/a.out" ||
  { cat "$tmp/a.out"; fail "(a) input file line"; }
bins=$(section "$tmp/a.out" "Range histogram (10 nm bins)" | grep -cE '^ +[0-9]+-[0-9]+ nm \| +[1-9][0-9]* obs \|')
[ "$bins" -ge 3 ] || { cat "$tmp/a.out"; fail "(a) $bins non-empty range bins, want >= 3"; }
az=$(section "$tmp/a.out" "Coverage by azimuth")
rows=$(grep -cE '^  [0-9]{3}-[0-9]{3} +[0-9]+ +[0-9]+ +[0-9.-]+$' <<<"$az")
[ "$rows" -eq 12 ] || { echo "$az"; fail "(a) $rows azimuth table rows, want 12"; }
bars=$(grep -cE '^  [0-9]{3}-[0-9]{3} \| ' <<<"$az")
[ "$bars" -eq 12 ] || { echo "$az"; fail "(a) $bars azimuth bar rows, want 12"; }
for s in "Overview" "Aircraft" "Range histogram" "Altitude histogram" "Elevation histogram" \
         "Coverage by azimuth" "Speed and vertical rate" "Gaps" "Top 15 aircraft" "Sent stream"; do
  grep -q "^== $s" "$tmp/a.out" || { cat "$tmp/a.out"; fail "(a) no '$s' section"; }
done
! grep -q 'skipped: receiver position unknown' "$tmp/a.out" || fail "(a) range sections skipped"

# ---- (b) byte-identical rerun ------------------------------------------------
"$stats" --lat 38.9 --lon -77.0 "$sample" >"$tmp/b.out" 2>&1 || fail "(b) exit $?"
cmp -s "$tmp/a.out" "$tmp/b.out" || { diff "$tmp/a.out" "$tmp/b.out"; fail "(b) output differs between runs"; }

# ---- (c) --json --------------------------------------------------------------
"$stats" --lat 38.9 --lon -77.0 --json "$sample" >"$tmp/c.json" 2>"$tmp/c.err" || { cat "$tmp/c.err"; fail "(c) exit"; }
python3 -I -c 'import json,sys; d=json.load(sys.stdin); assert d["aircraft"]["distinct"]==12' <"$tmp/c.json" ||
  fail "(c) aircraft.distinct"
python3 -I -c 'import json,sys
d = json.load(sys.stdin)
for k in ("overview","aircraft","range_histogram","altitude_histogram","elevation_histogram","azimuth",
          "speed","gaps","top_aircraft","sent"):
    assert k in d, k
assert d["overview"]["records"] == 1394, d["overview"]
assert d["overview"]["files"][0]["kind"] == "readsb-raw"
bins = d["range_histogram"]["bins"]
assert sum(1 for b in bins if b["obs"] > 0) >= 3
assert all(set(b) == {"bin_start","bin_end","obs","aircraft"} for b in bins)
assert len(d["azimuth"]["sectors"]) == 12
assert len(d["elevation_histogram"]["bins"]) == 18
assert isinstance(d["top_aircraft"], list) and len(d["top_aircraft"]) == 12
assert d["top_aircraft"][0]["obs"] >= d["top_aircraft"][-1]["obs"]
assert d["sent"] is None' <"$tmp/c.json" || { head -c 2000 "$tmp/c.json"; fail "(c) JSON structure"; }

# ---- (d) .gz input -----------------------------------------------------------
gzip -c "$sample" >"$tmp/sample.jsonl.gz"
"$stats" --lat 38.9 --lon -77.0 "$tmp/sample.jsonl.gz" >"$tmp/d.out" 2>"$tmp/d.err" || { cat "$tmp/d.err"; fail "(d) exit"; }
grep -qx 'Aircraft: 12' "$tmp/d.out" || { cat "$tmp/d.out"; fail "(d) .gz: no 'Aircraft: 12'"; }
# Apart from the file name, the same report as the plain file.
diff <(grep -v 'records$' "$tmp/a.out") <(grep -v 'records$' "$tmp/d.out") >/dev/null ||
  { diff "$tmp/a.out" "$tmp/d.out"; fail "(d) .gz report differs"; }
# A truncated .gz is an error (exit 1), not a shorter file.
gz_size=$(wc -c <"$tmp/sample.jsonl.gz")
head -c $((gz_size / 2)) "$tmp/sample.jsonl.gz" >"$tmp/half.jsonl.gz"
"$stats" --lat 38.9 --lon -77.0 "$tmp/half.jsonl.gz" >/dev/null 2>"$tmp/d2.err"; rc=$?
[ "$rc" -eq 1 ] || fail "(d) truncated .gz: exit $rc, want 1"
grep -q 'unexpected end of gzip stream' "$tmp/d2.err" || { cat "$tmp/d2.err"; fail "(d) truncated .gz message"; }

# ---- (e) no receiver position; then --config ----------------------------------
"$stats" "$sample" >"$tmp/e.out" 2>&1 || fail "(e) exit"
n=$(grep -c '^(skipped: receiver position unknown; pass --lat/--lon or --config)$' "$tmp/e.out")
[ "$n" -eq 3 ] || { cat "$tmp/e.out"; fail "(e) $n skip notes, want 3"; }
"$stats" --json "$sample" | python3 -I -c 'import json,sys; d=json.load(sys.stdin)
assert d["range_histogram"] is None and d["elevation_histogram"] is None and d["azimuth"] is None
assert d["altitude_histogram"] is not None' || fail "(e) --json without receiver"
printf '[receiver]\nlat = 38.9\nlon = -77.0\n' >"$tmp/rx.toml"
"$stats" --config "$tmp/rx.toml" "$sample" >"$tmp/e2.out" 2>&1 || { cat "$tmp/e2.out"; fail "(e2) exit"; }
grep -q '^Receiver: 38.900000, -77.000000 (from --config' "$tmp/e2.out" || { cat "$tmp/e2.out"; fail "(e2) receiver"; }
diff <(grep -v '^Receiver:' "$tmp/a.out") <(grep -v '^Receiver:' "$tmp/e2.out") >/dev/null ||
  { diff "$tmp/a.out" "$tmp/e2.out"; fail "(e2) --config report differs from --lat/--lon"; }

# ---- (f) usage errors --------------------------------------------------------
"$stats" >/dev/null 2>&1; rc=$?
[ "$rc" -eq 2 ] || fail "(f) no FILE: exit $rc, want 2"
"$stats" --lat 38.9 "$sample" >/dev/null 2>&1; rc=$?
[ "$rc" -eq 2 ] || fail "(f) --lat without --lon: exit $rc, want 2"
"$stats" "$tmp/missing.jsonl" >/dev/null 2>&1; rc=$?
[ "$rc" -eq 1 ] || fail "(f) missing file: exit $rc, want 1"

# ---- (g) recordings from a real skycued ----------------------------------------
# cues.track_timeout_s = 1: once the replay ends the tracks drop (source_lost) quickly.
cat >"$tmp/skycue.toml" <<EOF
[receiver]
lat = 38.9
lon = -77.0
[cues]
tcp_port = 0
track_timeout_s = 1
[web]
port = 0
[recording]
dir = "$tmp/rec"
formats = ["jsonl"]
EOF
"$skycued" --replay "$sample" --replay-speed 50 --config "$tmp/skycue.toml" >"$tmp/skycued.log" 2>&1 &
pid=$!
bg_pids+=($pid)
slow_sleep 5
kill "$pid" 2>/dev/null
for (( i=0; i<50*slowf; i++ )); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
! kill -0 "$pid" 2>/dev/null || fail "(g) skycued did not stop"
received=$(ls "$tmp"/rec/received-*.jsonl 2>/dev/null | head -n 1)
sentf=$(ls "$tmp"/rec/sent-*.jsonl 2>/dev/null | head -n 1)
[ -s "$received" ] && [ -s "$sentf" ] || { ls -l "$tmp/rec"; fail "(g) no recordings"; }

"$stats" --lat 38.9 --lon -77.0 "$sentf" >"$tmp/g.out" 2>"$tmp/g.err" || { cat "$tmp/g.err"; fail "(g) exit"; }
grep -qE '^  [^ ]*/sent-[0-9-]+\.jsonl +sent +[0-9]+ records$' "$tmp/g.out" || { cat "$tmp/g.out"; fail "(g) file kind"; }
grep -q '^Drops by reason' "$tmp/g.out" || { cat "$tmp/g.out"; fail "(g) no 'Drops by reason'"; }
grep -q '^Keyframe events: 0 ' "$tmp/g.out" || { cat "$tmp/g.out"; fail "(g) keyframe events"; }
grep -qE '^Filter changes: [1-9]' "$tmp/g.out" || { cat "$tmp/g.out"; fail "(g) filter changes"; }

"$stats" --lat 38.9 --lon -77.0 "$received" "$sentf" >"$tmp/h.out" 2>"$tmp/h.err" || { cat "$tmp/h.err"; fail "(h) exit"; }
grep -qE '^Compression: [0-9]+ received obs / [0-9]+ updates = [0-9]+\.[0-9]{2}$' "$tmp/h.out" ||
  { cat "$tmp/h.out"; fail "(h) no compression ratio"; }
grep -qx 'Aircraft: 12' "$tmp/h.out" || { cat "$tmp/h.out"; fail "(h) aircraft"; }
grep -qE '^Stream kinds: received, sent$' "$tmp/h.out" || { cat "$tmp/h.out"; fail "(h) stream kinds"; }
"$stats" --json "$received" "$sentf" | python3 -I -c 'import json,sys; d=json.load(sys.stdin)
s = d["sent"]
assert s["updates"] > 0 and s["keyframes"] == 0, s
assert sum(s["drops"].values()) > 0, s["drops"]
assert s["compression"] >= 1.0, s' || fail "(h) --json sent section"

# ---- (i) FIS-B section: only with uat_uplink records -------------------------
grep -q '^== FIS-B' "$tmp/a.out" && fail "(i) FIS-B section on a recording without uplinks"
"$stats" --json "$sample" | python3 -I -c 'import json,sys; assert json.load(sys.stdin)["fisb"] is None' ||
  fail "(i) --json fisb not null without uplinks"
uplinks="$fixtures/fisb/received-with-uplinks.jsonl"
# The capture-derived recording is fetched by scripts/fixtures/fetch-test-captures.sh.
if [ -e "$uplinks" ]; then
"$stats" "$uplinks" >"$tmp/i.out" 2>"$tmp/i.err" || { cat "$tmp/i.err"; fail "(i) exit"; }
fs=$(section "$tmp/i.out" "FIS-B products by ID")
# The 2015 sample's counts (skycue-fisb --summary on the same file agrees).
grep -qx 'Uplinks: 704, APDUs: 563, products decoded: 561' <<<"$fs" || { echo "$fs"; fail "(i) uplink counts"; }
grep -qE '^ +413  generic text +224$' <<<"$fs" || { echo "$fs"; fail "(i) product 413"; }
grep -qE '^ +8  NOTAM +62$' <<<"$fs" || { echo "$fs"; fail "(i) product 8"; }
grep -qE '^ +63  NEXRAD regional +200$' <<<"$fs" || { echo "$fs"; fail "(i) product 63"; }
grep -qE '^  metar  decoded +150  stored +127$' <<<"$fs" || { echo "$fs"; fail "(i) METAR"; }
grep -qE '^ +8  NOTAM +27 text, 6 keep-alive, 29 graphics, 0 cancel; stored 23 \(0 cancelled\)$' <<<"$fs" ||
  { echo "$fs"; fail "(i) TWGO 8"; }
grep -qE '^  63 +NEXRAD regional +2015-01-24T04:10:00\.000Z  671 blocks, published 1$' <<<"$fs" ||
  { echo "$fs"; fail "(i) mosaic 63"; }
grep -qE '^  bad_frames +0$' <<<"$fs" || { echo "$fs"; fail "(i) counters"; }
"$stats" "$uplinks" | cmp -s - "$tmp/i.out" || fail "(i) rerun differs"
"$stats" --json "$uplinks" | python3 -I -c 'import json,sys; f=json.load(sys.stdin)["fisb"]
assert f["uplinks"] == 704 and f["apdus"] == 563 and f["products"] == 561, f
assert f["products_by_id"]["413"] == 224 and f["products_by_id"]["8"] == 62, f["products_by_id"]
assert f["text"]["metar"] == {"decoded": 150, "stored": 127}, f["text"]
t8 = [t for t in f["twgo"] if t["product"] == 8][0]
assert (t8["text"], t8["keepalive"], t8["graphics"], t8["stored"]) == (27, 6, 29, 23), t8
m = f["mosaics"][0]
assert (m["key"], m["blocks"], m["published"]) == ("63", 671, 1), m
assert f["counters"]["store.applied"] == 561, f["counters"]' || fail "(i) --json fisb section"
else
  echo "SKIP (i): $uplinks not fetched: run scripts/fixtures/fetch-test-captures.sh"
fi

echo "PASS"
