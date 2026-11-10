#!/usr/bin/env bash
# Tests scripts/test-data/gen-capture-seeds.py and its use by scripts/dev/run-fuzz.sh.
#
# The generator needs the fetched captures (tests/data/raw/*.gz,
# fisb/tg30.978, fisb/received-with-uplinks.jsonl; override the directory with
# SKYCUE_TEST_DATA). Checks:
#   - an empty test data directory: exit 3, nothing written;
#   - with the captures: two runs are byte-identical, and each target gets
#     exactly the seeds docs/hardening.md lists;
#   - while tests/fuzz/seeds still holds the committed copies: every generated file
#     is byte-identical to the committed seed of the same name;
#   - run-fuzz.sh (with stub harnesses, no clang) puts committed and generated
#     seeds into each corpus, prints a SKIP line for a missing generated set
#     and refuses a target without any committed seed.
# Exit 77 when the captures are not present (nothing to generate from).
set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
gen="$repo/scripts/test-data/gen-capture-seeds.py"
td=${SKYCUE_TEST_DATA:-$repo/tests/data}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fails=0

ok() { echo "ok: $*"; }
bad() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }
check() { # description, command...
  local d=$1
  shift
  if "$@" > /dev/null 2>&1; then ok "$d"; else bad "$d"; fi
}
count() { find "$1" -type f 2> /dev/null | wc -l; }

[ -f "$gen" ] || { bad "scripts/test-data/gen-capture-seeds.py missing"; exit 1; }

# ---- empty test data directory: exit 3, nothing written ----
mkdir -p "$tmp/empty"
python3 -I "$gen" "$tmp/empty" "$tmp/out-empty" > "$tmp/empty.out" 2>&1
rc=$?
[ "$rc" = 3 ] && ok "empty test data directory exits 3" || bad "empty test data directory exit $rc, want 3"
[ "$(count "$tmp/out-empty")" = 0 ] && ok "empty test data directory writes nothing" || bad "empty test data directory wrote files"
grep -q 'missing' "$tmp/empty.out" && ok "empty test data directory says what is missing" || bad "no 'missing' message"

# ---- one capture missing: that target only is skipped ----
if [ -f "$td/fisb/tg30.978" ] && [ -f "$td/raw/uat-sample.txt.gz" ]; then
  mkdir -p "$tmp/partial/fisb"
  cp "$td/fisb/tg30.978" "$tmp/partial/fisb/"
  python3 -I "$gen" "$tmp/partial" "$tmp/out-partial" > /dev/null 2>&1
  rc=$?
  [ "$rc" = 3 ] && ok "partial test data exit 3" || bad "partial test data exit $rc"
  [ "$(count "$tmp/out-partial/fisb_decoder")" = 12 ] && [ ! -e "$tmp/out-partial/raw_formats" ] \
    && ok "partial test data: fisb_decoder written, raw_formats not" || bad "partial test data output wrong"
fi

# ---- the stub harness tree for run-fuzz ----
stubs="$tmp/bin"
mkdir -p "$stubs"
for t in raw_formats fisb_decoder iqgen_coding recording; do
  cat > "$stubs/fuzz_$t" << 'STUB'
#!/usr/bin/env bash
# libFuzzer stand-in: lists the corpus (last argument) and exits.
for a; do c=$a; done
ls "$c" > "$STUB_LIST.${0##*/}"
exit 0
STUB
  chmod +x "$stubs/fuzz_$t"
done

# A seeds tree as it is after the capture-derived seeds are removed.
stripped="$tmp/seeds"
cp -r "$repo/tests/fuzz/seeds" "$stripped"
rm -f "$stripped"/raw_formats/seed-{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18}.txt
rm -f "$stripped"/fisb_decoder/seed-{1,2,3,4,5,6}.bin "$stripped"/fisb_decoder/seed-{7,8,9,10,11,12}.txt
rm -f "$stripped"/iqgen_coding/seed-{1,2}.bin
rm -f "$stripped"/recording/seed-{1,2,3,4,5,6}.jsonl

runfuzz() { # work-name, test data directory, extra args...
  local w=$1 f=$2
  shift 2
  STUB_LIST="$tmp/$w.list" bash "$repo/scripts/dev/run-fuzz.sh" all --seconds 1 \
    --bin-dir "$stubs" --work "$tmp/$w" --findings "$tmp/$w-findings" \
    --seeds "$stripped" --test-data "$f" "$@" > "$tmp/$w.out" 2>&1
}
listed() { wc -l < "$tmp/$1.list.fuzz_$2"; }

# Without captures every target still starts from its committed seeds and a
# SKIP line names each missing generated set. Counts after the capture-derived
# seeds are gone: raw_formats 19-24, fisb_decoder one hand-made seed,
# iqgen_coding 3-6, recording 7-10.
runfuzz notd "$tmp/empty"
rc=$?
[ "$rc" = 0 ] && ok "run-fuzz without captures exits 0" || { bad "run-fuzz without captures exit $rc"; cat "$tmp/notd.out" >&2; }
for p in raw_formats:6 fisb_decoder:1 iqgen_coding:4 recording:4; do
  t=${p%%:*} n=${p##*:}
  [ "$(listed notd "$t")" = "$n" ] && ok "no captures: $t starts with $n committed seeds" \
    || bad "no captures: $t has $(listed notd "$t") seeds, want $n"
  grep -q "SKIP.*$t" "$tmp/notd.out" && ok "no captures: SKIP line for $t" || bad "no captures: no SKIP line for $t"
done

# A target with no committed seed is an error.
mkdir -p "$tmp/seeds-none"
cp -r "$stripped"/. "$tmp/seeds-none/"
rm -f "$tmp/seeds-none"/fisb_decoder/*
STUB_LIST="$tmp/none.list" bash "$repo/scripts/dev/run-fuzz.sh" fisb_decoder --seconds 1 --bin-dir "$stubs" \
  --work "$tmp/none" --findings "$tmp/none-findings" --seeds "$tmp/seeds-none" --test-data "$tmp/empty" \
  > "$tmp/none.out" 2>&1
rc=$?
if [ "$rc" = 1 ] && grep -q 'no committed seed' "$tmp/none.out"; then
  ok "target without a committed seed fails"
else
  bad "target without a committed seed: exit $rc"
fi

# ---- with the captures ----
if [ ! -f "$td/raw/uat-sample.txt.gz" ] || [ ! -f "$td/raw/stratux-uat-2015.log.gz" ] \
  || [ ! -f "$td/raw/modes-120s.csv.gz" ] || [ ! -f "$td/fisb/tg30.978" ] \
  || [ ! -f "$td/fisb/received-with-uplinks.jsonl" ]; then
  echo "SKIP: the captures are not fetched (run scripts/test-data/fetch-test-captures.sh); nothing to generate from" >&2
  [ "$fails" = 0 ] || exit 1
  exit 77
fi

python3 -I "$gen" "$td" "$tmp/a" > /dev/null 2>&1 && ok "generator exits 0 with the captures" || bad "generator failed"
python3 -I "$gen" "$td" "$tmp/b" > /dev/null 2>&1
check "two runs are byte-identical" diff -r "$tmp/a" "$tmp/b"
for p in raw_formats:18 fisb_decoder:12 iqgen_coding:2 recording:6; do
  t=${p%%:*} n=${p##*:}
  [ "$(count "$tmp/a/$t")" = "$n" ] && ok "$t has $n seeds" || bad "$t has $(count "$tmp/a/$t") seeds, want $n"
done
[ "$(ls "$tmp/a" | wc -l)" = 4 ] && ok "only the four capture targets" || bad "unexpected targets: $(ls "$tmp/a")"

# Oracle: the committed copies, while they exist.
compared=0
for f in "$tmp"/a/*/*; do
  rel=${f#"$tmp/a/"}
  [ -f "$repo/tests/fuzz/seeds/$rel" ] || continue
  compared=$((compared + 1))
  cmp -s "$f" "$repo/tests/fuzz/seeds/$rel" || bad "differs from the committed seed: $rel"
done
if [ "$compared" -gt 0 ]; then
  ok "$compared generated seeds byte-identical to the committed ones"
else
  echo "note: no committed capture-derived seeds left to compare against"
fi

# run-fuzz with captures: committed + generated beside each other.
runfuzz withtd "$td"
rc=$?
[ "$rc" = 0 ] && ok "run-fuzz with captures exits 0" || { bad "run-fuzz with captures exit $rc"; cat "$tmp/withtd.out" >&2; }
for p in raw_formats:24 fisb_decoder:13 iqgen_coding:6 recording:10; do
  t=${p%%:*} n=${p##*:}
  [ "$(listed withtd "$t")" = "$n" ] && ok "captures: $t corpus starts with $n seeds" \
    || bad "captures: $t has $(listed withtd "$t") seeds, want $n"
done
grep -q SKIP "$tmp/withtd.out" && bad "SKIP line although the captures are present"

[ "$fails" = 0 ] || { echo "$fails check(s) failed" >&2; exit 1; }
echo "all checks passed"
