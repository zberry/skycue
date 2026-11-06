#!/usr/bin/env bash
# make-locations.py: builds the optional LOCATIONS_FILE from small
# OurAirports-style excerpts written here (no real data is committed).
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
script="$here/scripts/maps/make-locations.py"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: test_make_locations: $*"; [ -f "$tmp/out" ] && cat "$tmp/out"; [ -f "$tmp/err" ] && cat "$tmp/err"; exit 1; }
[ -f "$script" ] || fail "scripts/maps/make-locations.py missing"
command -v python3 >/dev/null || { echo "SKIP test_make_locations: no python3"; exit 77; }

cat > "$tmp/airports.csv" <<'EOF'
"id","ident","type","name","latitude_deg","longitude_deg","elevation_ft","continent","iso_country","iso_region","municipality","scheduled_service","gps_code","iata_code","local_code","home_link","wikipedia_link","keywords"
1,"KIND","large_airport","Indianapolis Intl",39.7173,-86.2944,797,"NA","US","US-IN","Indianapolis","yes","KIND","IND","IND","","",""
2,"KPRG","small_airport","Edgar County",39.6991,-87.6702,700,"NA","US","US-IL","Paris","no","KPRG","","PRG","","",""
3,"00XX","closed","Gone Field",30.0,-90.0,10,"NA","US","US-LA","Nowhere","no","","","","","",""
4,"X1Y","heliport","Some Heliport",10.0,10.0,10,"NA","US","US-LA","Nowhere","no","","","","","",""
5,"BOS","small_airport","Boston Dup",42.36,-71.01,10,"NA","US","US-MA","Boston","no","","","","","",""
6,"KNOP","small_airport","No Position",,,10,"NA","US","US-IN","Nowhere","no","KNOP","","","","",""
EOF
cat > "$tmp/navaids.csv" <<'EOF'
"id","filename","ident","name","type","frequency_khz","latitude_deg","longitude_deg"
1,"Indy","IND","Indianapolis","VORTAC",112500,39.81,-86.37
2,"Rod","ROD","Rosewood","VORTAC",114100,40.29,-84.04
3,"BosA","BOS","Boston","VOR",112700,42.36,-71.01
4,"BosB","BOS","Mauritius","VOR",112700,-20.0,57.5
5,"Long","TOOLONG","Bad Id","NDB",350,41.0,-85.0
EOF

run() { python3 -I "$script" "$@" >"$tmp/out" 2>"$tmp/err"; rc=$?; }

cat > "$tmp/expected.csv" <<'EOF'
IND,39.810000,-86.370000
KIND,39.717300,-86.294400
KPRG,39.699100,-87.670200
ROD,40.290000,-84.040000
X1Y,10.000000,10.000000
EOF

run "$tmp/airports.csv" "$tmp/navaids.csv"
[ "$rc" = 0 ] || fail "exit $rc, want 0"
cmp -s "$tmp/out" "$tmp/expected.csv" || { diff "$tmp/expected.csv" "$tmp/out"; fail "output differs"; }
grep -q "1 ambiguous dropped" "$tmp/err" || fail "stderr lacks '1 ambiguous dropped'"
grep -q "1 closed skipped" "$tmp/err" || fail "stderr lacks '1 closed skipped'"
# KNOP (no position) and TOOLONG (a 7-character id): two rows dropped.
grep -qx "make-locations: 5 locations (3 airports, 2 navaids), 1 ambiguous dropped, 1 closed skipped, 2 invalid skipped" \
  "$tmp/err" || fail "summary line"

# -o writes the same bytes and prints nothing on stdout.
run "$tmp/airports.csv" "$tmp/navaids.csv" -o "$tmp/out.csv"
[ "$rc" = 0 ] || fail "-o: exit $rc"
[ ! -s "$tmp/out" ] || fail "-o: stdout not empty"
cmp -s "$tmp/out.csv" "$tmp/expected.csv" || fail "-o: bytes differ"

# CRLF input and a BOM are tolerated.
printf '\357\273\277' > "$tmp/crlf.csv"
sed 's/$/\r/' "$tmp/navaids.csv" >> "$tmp/crlf.csv"
run "$tmp/airports.csv" "$tmp/crlf.csv"
[ "$rc" = 0 ] || fail "crlf: exit $rc"
cmp -s "$tmp/out" "$tmp/expected.csv" || fail "crlf: output differs"

# A missing required column: exit 2 naming the column.
sed '1s/"latitude_deg"/"lat"/' "$tmp/navaids.csv" > "$tmp/bad.csv"
run "$tmp/airports.csv" "$tmp/bad.csv"
[ "$rc" = 2 ] || fail "missing column: exit $rc, want 2"
grep -q "latitude_deg" "$tmp/err" || fail "message does not name latitude_deg"
grep -q "bad.csv" "$tmp/err" || fail "message does not name the file"

# Usage error: exit 2. Unreadable file: exit 1.
run "$tmp/airports.csv"
[ "$rc" = 2 ] || fail "usage: exit $rc, want 2"
run "$tmp/airports.csv" "$tmp/nope.csv"
[ "$rc" = 1 ] || fail "missing file: exit $rc, want 1"

# --help: the usage on stdout, exit 0.
run --help
[ "$rc" = 0 ] || fail "--help: exit $rc, want 0"
grep -q "AIRPORTS_CSV NAVAIDS_CSV" "$tmp/out" || fail "--help: no usage on stdout"

# The cap the script warns at is skycued's (kMaxOperatorRows).
hpp="$here/skycued/src/fisb/location_index.hpp"
daemon_cap=$(sed -nE 's/^constexpr std::size_t kMaxOperatorRows = ([0-9]+);.*/\1/p' "$hpp")
script_cap=$(sed -nE 's/^DAEMON_MAX_ROWS = ([0-9]+).*/\1/p' "$script")
[ -n "$daemon_cap" ] && [ "$daemon_cap" = "$script_cap" ] || fail "cap: script $script_cap, skycued $daemon_cap"

# Past the cap: the rows are still written, with a warning naming the cap.
python3 -I - "$tmp/big.csv" "$daemon_cap" <<'EOP' || fail "big.csv"
import sys
path, cap = sys.argv[1], int(sys.argv[2])
digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
with open(path, "w") as f:
    f.write('"id","ident","type","latitude_deg","longitude_deg"\n')
    for i in range(cap + 1):
        ident, n = "Q", i
        for _ in range(4):
            ident, n = ident + digits[n % 36], n // 36
        f.write('%d,"%s","small_airport",40.0,-86.0\n' % (i, ident))
EOP
printf '"id","ident","latitude_deg","longitude_deg"\n' > "$tmp/nonav.csv"
run "$tmp/big.csv" "$tmp/nonav.csv" -o "$tmp/big-out.csv"
[ "$rc" = 0 ] || fail "big: exit $rc"
[ "$(wc -l < "$tmp/big-out.csv")" = $((daemon_cap + 1)) ] || fail "big: row count"
grep -q "^make-locations: $((daemon_cap + 1)) locations" "$tmp/err" || fail "big: summary count"
grep -q "warning: $((daemon_cap + 1)) rows is more than the $daemon_cap skycued reads" "$tmp/err" || fail "big: no cap warning"
run "$tmp/airports.csv" "$tmp/navaids.csv"
grep -q "warning" "$tmp/err" && fail "warning under the cap"

echo "PASS test_make_locations"
