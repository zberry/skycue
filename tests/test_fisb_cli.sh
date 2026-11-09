#!/usr/bin/env bash
# skycue-fisb on the FIS-B test data: --summary of the 2015 dump978 sample,
# --json of tg30.978 against the golden's SHA-256, --ref-date, .gz, the three
# input kinds (dump978 lines, rawrec captures, received JSONL), byte-identical
# reruns and the exit codes.
# usage: test_fisb_cli.sh SKYCUE_FISB DATA_DIR
set -u
fisb=$1 data_dir=$2
sample="$data_dir/raw/uat-sample.txt.gz"
tg30="$data_dir/fisb/tg30.978"
golden_sha="$data_dir/fisb/tg30.golden.sha256"
received="$data_dir/fisb/received-with-uplinks.jsonl"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*"; exit 1; }
# The captures are fetched by scripts/test-data/fetch-test-captures.sh (data/README.md).
for f in "$sample" "$tg30" "$received"; do
  [ -e "$f" ] || { echo "SKIP: $f not fetched: run scripts/test-data/fetch-test-captures.sh"; exit 77; }
done
for f in "$fisb" "$golden_sha"; do [ -e "$f" ] || fail "$f missing"; done

# section FILE TITLE: the lines of one "== TITLE..." section, header excluded.
section() { awk -v t="== $2" 'index($0, t) == 1 { on = 1; next } /^== / { on = 0 } on' "$1"; }

# ---- (a) --summary on the 2015 sample ----------------------------------------
"$fisb" --ref-date 2015-01-24 "$sample" >"$tmp/a.out" 2>"$tmp/a.err" || { cat "$tmp/a.err"; fail "(a) exit $?"; }
[ -s "$tmp/a.err" ] && { cat "$tmp/a.err"; fail "(a) unexpected stderr"; }
grep -qE '^  [^ ]*uat-sample\.txt\.gz +dump978 +1143 lines: 704 uplinks, 439 downlinks, 0 other$' "$tmp/a.out" ||
  { cat "$tmp/a.out"; fail "(a) input file line"; }
products=$(section "$tmp/a.out" "Products by ID")
for p in 8 11 12 13 63 413; do
  grep -qE "^ +$p +" <<<"$products" || { echo "$products"; fail "(a) product $p not listed"; }
done
for p in 14 64 70 103; do
  ! grep -qE "^ +$p +" <<<"$products" || { echo "$products"; fail "(a) product $p listed"; }
done
grep -qx '   63  NEXRAD regional: 200 messages, 0 non-empty (RLE) blocks, 200 empty-block lists with 1342 empty blocks' \
  "$tmp/a.out" || { section "$tmp/a.out" "Global blocks"; fail "(a) regional radar line"; }
grep -qE '^ +63 +NEXRAD regional +2015-01-24T04:10:00\.000Z +671 blocks, 671 empty ' "$tmp/a.out" ||
  { section "$tmp/a.out" "Mosaics"; fail "(a) regional mosaic line"; }
[ "$(section "$tmp/a.out" "Ground stations" | grep -cE '^ +-?[0-9]+\.[0-9]{4} +-?[0-9]+\.[0-9]{4} ')" -eq 1 ] ||
  { section "$tmp/a.out" "Ground stations"; fail "(a) want 1 ground station"; }
grep -qE '^  metar +decoded +150 +stored +127 ' "$tmp/a.out" || { section "$tmp/a.out" "Text"; fail "(a) METAR counts"; }
for s in "Inputs" "Uplinks" "Products by ID" "Text records" "TWGO records" "Global blocks" "Mosaics" \
         "Ground stations" "Store events" "Decoder counters"; do
  grep -q "^== $s" "$tmp/a.out" || { cat "$tmp/a.out"; fail "(a) no '$s' section"; }
done

# ---- (b) byte-identical reruns -----------------------------------------------
"$fisb" --ref-date 2015-01-24 "$sample" >"$tmp/b.out" 2>&1 || fail "(b) exit $?"
cmp -s "$tmp/a.out" "$tmp/b.out" || { diff "$tmp/a.out" "$tmp/b.out"; fail "(b) summary differs between runs"; }

# ---- (c) --json on tg30 has the golden's SHA-256 -----------------------------
"$fisb" --json --ref-date 2020-10-30 "$tg30" >"$tmp/c1.jsonl" 2>"$tmp/c.err" || { cat "$tmp/c.err"; fail "(c) exit"; }
[ -s "$tmp/c.err" ] && { cat "$tmp/c.err"; fail "(c) unexpected stderr"; }
"$fisb" --json "$tg30" >"$tmp/c2.jsonl" 2>"$tmp/c2.err" || fail "(c) exit without --ref-date"
cmp -s "$tmp/c1.jsonl" "$tmp/c2.jsonl" || fail "(c) output differs between runs (or --ref-date changed t= lines)"
[ -s "$tmp/c2.err" ] && { cat "$tmp/c2.err"; fail "(c) warned with every line timed"; }
# Every time in the output (received included) derives from the t= stamps: exact comparison,
# by hash (docs/fisb-golden.md says how to refresh it).
want=$(tr -d ' \n' <"$golden_sha")
got=$(sha256sum "$tmp/c1.jsonl" | cut -d' ' -f1)
if [ "$got" != "$want" ]; then
  trap - EXIT  # keep $tmp so the output can be inspected
  echo "(c) output kept at $tmp/c1.jsonl"
  fail "(c) --json SHA-256 is $got, tg30.golden.sha256 has $want"
fi
python3 -I -c 'import json,sys
n = 0
for line in sys.stdin:
    d = json.loads(line); n += 1
    assert d["type"] in ("fisb_text","fisb_twgo","fisb_radar","fisb_raster","fisb_expired"), d["type"]
assert n > 100, n' <"$tmp/c1.jsonl" || fail "(c) --json lines are not FIS-B records"

# ---- (d) --ref-date sets the date of timestamp-less lines ---------------------
"$fisb" --json --ref-date 2015-01-24 "$sample" >"$tmp/d1.jsonl" 2>/dev/null || fail "(d) exit (24th)"
"$fisb" --json --ref-date 2015-01-10 "$sample" >"$tmp/d2.jsonl" 2>/dev/null || fail "(d) exit (10th)"
grep -q '"type":"fisb_radar".*"product_time":"2015-01-24T04:10:00.000Z"' "$tmp/d1.jsonl" || fail "(d) no 01-24 radar"
grep -q '"type":"fisb_radar".*"product_time":"2015-01-10T04:10:00.000Z"' "$tmp/d2.jsonl" || fail "(d) no 01-10 radar"
"$fisb" --summary "$sample" >/dev/null 2>"$tmp/d3.err" || fail "(d) exit without --ref-date"
grep -q 'WARN.*--ref-date' "$tmp/d3.err" || { cat "$tmp/d3.err"; fail "(d) no warning without --ref-date"; }

# ---- (e) .gz, rawrec captures and received JSONL ------------------------------
gzip -c "$tg30" >"$tmp/tg30.978.gz"
"$fisb" --json "$tmp/tg30.978.gz" >"$tmp/e1.jsonl" 2>/dev/null || fail "(e) exit (.gz)"
cmp -s "$tmp/c1.jsonl" "$tmp/e1.jsonl" || fail "(e) .gz output differs from plain"
# The received sample holds the sample's uplinks at 1 Hz from 2015-01-24T04:12:00Z;
# the same uplinks as a rawrec capture on the same clock must decode identically.
gzip -dc "$sample" | grep '^+' | awk '{ printf "%d %s\n", 1422072720000 + (NR - 1) * 1000, $0 }' >"$tmp/uat-x.txt"
"$fisb" --json "$received" >"$tmp/e2.jsonl" 2>"$tmp/e2.err" || { cat "$tmp/e2.err"; fail "(e) exit (received)"; }
"$fisb" --json "$tmp/uat-x.txt" >"$tmp/e3.jsonl" 2>"$tmp/e3.err" || { cat "$tmp/e3.err"; fail "(e) exit (rawrec)"; }
[ -s "$tmp/e2.err" ] || [ -s "$tmp/e3.err" ] && { cat "$tmp/e2.err" "$tmp/e3.err"; fail "(e) unexpected stderr"; }
[ "$(wc -l <"$tmp/e2.jsonl")" -gt 100 ] || fail "(e) received: too few records"
cmp -s "$tmp/e2.jsonl" "$tmp/e3.jsonl" || { diff "$tmp/e2.jsonl" "$tmp/e3.jsonl" | head -c 2000; fail "(e) rawrec != received"; }
"$fisb" "$received" "$tmp/uat-x.txt" "$tmp/tg30.978.gz" >"$tmp/e4.out" 2>/dev/null || fail "(e) exit (summary of three)"
grep -qE 'received-with-uplinks\.jsonl +received +1065 lines: 704 uplinks, 0 downlinks, 361 other$' "$tmp/e4.out" ||
  { section "$tmp/e4.out" "Inputs"; fail "(e) received input line"; }
grep -qE 'uat-x\.txt +rawrec +704 lines: 704 uplinks, 0 downlinks, 0 other$' "$tmp/e4.out" ||
  { section "$tmp/e4.out" "Inputs"; fail "(e) rawrec input line"; }
grep -qE 'tg30\.978\.gz +dump978-t +1048 lines: 963 uplinks, 0 downlinks, 85 other$' "$tmp/e4.out" ||
  { section "$tmp/e4.out" "Inputs"; fail "(e) tg30 input line"; }

# ---- (g) undated input without radar: anchored on the median product-413 time ----
# Keep the sample uplinks whose products are only product 413 (no global blocks).
gzip -dc "$sample" | grep '^+' >"$tmp/up.txt"
: >"$tmp/text-only.txt"
while IFS= read -r line; do
  printf '%s\n' "$line" >"$tmp/one.txt"
  "$fisb" --ref-date 2015-01-24 "$tmp/one.txt" >"$tmp/one.out" 2>/dev/null || continue
  ids=$(section "$tmp/one.out" "Products by ID" | awk 'NF { print $1 }' | sort -u | tr '\n' ' ')
  [ "$ids" = "413 " ] && printf '%s\n' "$line" >>"$tmp/text-only.txt"
done <"$tmp/up.txt"
n=$(wc -l <"$tmp/text-only.txt")
[ "$n" -ge 5 ] || fail "(g) only $n text-only uplinks in the sample"
"$fisb" --ref-date 2015-01-24 "$tmp/text-only.txt" >"$tmp/g.out" 2>"$tmp/g.err" || { cat "$tmp/g.err"; fail "(g) exit"; }
grep -qE '^  undated taken as   2015-01-2[34]T[0-9:.]+Z \(the median product-413 time on --ref-date 2015-01-24\)$' "$tmp/g.out" ||
  { section "$tmp/g.out" "Uplinks"; fail "(g) not anchored on the median 413 time"; }
[ -z "$(section "$tmp/g.out" "Global blocks" | grep messages)" ] || fail "(g) global blocks in the text-only file"
grep -qE '^  metar +decoded +[1-9]' "$tmp/g.out" || { section "$tmp/g.out" "Text records"; fail "(g) no METARs"; }

# ---- (h) a truncated .gz is an error ------------------------------------------
size=$(wc -c <"$sample")
head -c $((size / 2)) "$sample" >"$tmp/half.txt.gz"
"$fisb" --ref-date 2015-01-24 "$tmp/half.txt.gz" >/dev/null 2>"$tmp/h.err"; rc=$?
[ $rc -eq 1 ] || fail "(h) truncated .gz: exit $rc, want 1"
grep -q 'unexpected end of gzip stream' "$tmp/h.err" || { cat "$tmp/h.err"; fail "(h) message"; }
head -c $((size - 4)) "$sample" >"$tmp/trailer.txt.gz"  # only the CRC/length trailer missing
"$fisb" --json --ref-date 2015-01-24 "$tmp/trailer.txt.gz" >/dev/null 2>&1; rc=$?
[ $rc -eq 1 ] || fail "(h) .gz without its trailer: exit $rc, want 1"

# ---- (f) exit codes ------------------------------------------------------------
"$fisb" >/dev/null 2>&1; rc=$?; [ $rc -eq 2 ] || fail "(f) no FILE: exit $rc, want 2"
"$fisb" --ref-date 2015-13-01 "$tg30" >/dev/null 2>&1; rc=$?; [ $rc -eq 2 ] || fail "(f) bad --ref-date: exit $rc, want 2"
"$fisb" --ref-date 15-01-24 "$tg30" >/dev/null 2>&1; rc=$?; [ $rc -eq 2 ] || fail "(f) short --ref-date: exit $rc, want 2"
"$fisb" --json --summary "$tg30" >/dev/null 2>&1; rc=$?; [ $rc -eq 2 ] || fail "(f) --json --summary: exit $rc, want 2"
"$fisb" --bogus "$tg30" >/dev/null 2>&1; rc=$?; [ $rc -eq 2 ] || fail "(f) unknown option: exit $rc, want 2"
"$fisb" "$tmp/does-not-exist" >/dev/null 2>&1; rc=$?; [ $rc -eq 1 ] || fail "(f) missing file: exit $rc, want 1"
gzip -dc "$sample" | grep '^-' >"$tmp/down.txt"
"$fisb" --ref-date 2015-01-24 "$tmp/down.txt" >/dev/null 2>&1; rc=$?; [ $rc -eq 3 ] || fail "(f) no uplinks: exit $rc, want 3"
"$fisb" --help >/dev/null 2>&1; rc=$?; [ $rc -eq 0 ] || fail "(f) --help: exit $rc, want 0"

echo "PASS"
