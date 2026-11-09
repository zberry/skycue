#!/usr/bin/env bash
# skycue-iqgen end to end: help and usage errors, a 1 s Mode S slice (summary,
# sidecar, determinism, info), a 5 s slice's time and size, UAT (dump978 and
# stratux logs), resample
# (file and stdin), the input error paths, the size guard, no frames, and a
# write failure.
# usage: test_iqgen_cli.sh SKYCUE_IQGEN DATA_DIR
set -u
iqgen=$1 data_dir=$2
modes="$data_dir/raw/modes-120s.csv.gz"
uat="$data_dir/raw/uat-sample.txt.gz"
stratux="$data_dir/raw/stratux-uat-2015.log.gz"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*"; exit 1; }
# data/raw/ is fetched by scripts/test-data/fetch-test-captures.sh (data/README.md):
# without it the sections that read those captures are skipped and the
# checks on generated inputs still run.
have_raw=1
for f in "$modes" "$uat" "$stratux"; do
  [ -e "$f" ] || { have_raw=0; echo "SKIP the test data sections: $f not fetched: run scripts/test-data/fetch-test-captures.sh"; break; }
done
[ -e "$iqgen" ] || fail "$iqgen missing"
# A Mode S input for the usage and option errors, which do not need the capture.
any=$modes
if [ "$have_raw" = 0 ]; then
  any=$tmp/one.avr
  printf '@000000000000%s;\n' 8D4840D6202CC371C32CE0576098 >"$any"
fi

# expect_exit CODE NAME CMD...: runs CMD (stdout/stderr to $tmp/NAME.out/.err).
expect_exit() {
  local want=$1 name=$2; shift 2
  "$@" >"$tmp/$name.out" 2>"$tmp/$name.err"
  local got=$?
  [ "$got" -eq "$want" ] || { cat "$tmp/$name.err"; fail "$name: exit $got, want $want"; }
}
# field NAME KEY: the value of KEY=... in $tmp/NAME.out.
field() { tr ' ' '\n' <"$tmp/$1.out" | sed -n "s/^$2=//p"; }
size() { stat -c %s "$1"; }

# ---- help and usage errors ---------------------------------------------------
expect_exit 0 help "$iqgen" --help
grep -q '^usage: skycue-iqgen' "$tmp/help.out" || fail "--help: no usage line"
expect_exit 0 help_modes "$iqgen" modes --help
grep -q 'usage: skycue-iqgen' "$tmp/help_modes.out" || fail "modes --help: no usage line"
expect_exit 0 help_resample "$iqgen" resample --help
grep -q '32 taps' "$tmp/help_resample.out" || fail "resample --help does not mention the 32-tap limit"
grep -qF 'min(0.5 * IN_RATE, 0.45 * --out-rate)' "$tmp/help_resample.out" || fail "resample --help does not state the cutoff"
expect_exit 2 noargs "$iqgen"
expect_exit 2 unknown "$iqgen" frobnicate
grep -q "frobnicate" "$tmp/unknown.err" || fail "unknown command not named"
expect_exit 2 no_o "$iqgen" modes "$any"
expect_exit 2 badopt "$iqgen" modes "$any" -o "$tmp/x.cu8" --no-such-option
[ -e "$tmp/x.cu8" ] && fail "usage error wrote an output"

if [ "$have_raw" = 1 ]; then  # up to the stratux summary: data/raw/
# ---- modes: a 1 s slice -------------------------------------------------------
start=$(date +%s%N)
expect_exit 0 a "$iqgen" modes "$modes" --duration 1 -o "$tmp/a.1090.cu8"
elapsed_ms=$((($(date +%s%N) - start) / 1000000))
[ "$elapsed_ms" -lt 6000 ] || fail "modes --duration 1 took ${elapsed_ms} ms (>= 6 s)"
grep -qE '^frames=[0-9]+ expected=[0-9]+ overlaps=[0-9]+ clipped=[0-9]+ skipped=0 seconds=1\.[0-9]{3} bytes=[0-9]+ skipped_seconds=0\.000$' \
  "$tmp/a.out" || { cat "$tmp/a.out"; fail "modes summary line"; }
[ "$(wc -l <"$tmp/a.out")" -eq 1 ] || { cat "$tmp/a.out"; fail "modes: stdout is not one line"; }
frames=$(field a frames) bytes=$(field a bytes)
[ "$bytes" -eq "$(size "$tmp/a.1090.cu8")" ] || fail "bytes=$bytes but the file has $(size "$tmp/a.1090.cu8")"
side="$tmp/a.1090.frames.txt"
[ -f "$side" ] || fail "no sidecar a.1090.frames.txt"
[ "$(wc -l <"$side")" -eq "$frames" ] || fail "sidecar has $(wc -l <"$side") lines, frames=$frames"
bad=$(grep -cvE '^[0-9]+ [0-9A-F]{14}([0-9A-F]{14})? (expected|optional)( overlap)?$' "$side")
[ "$bad" -eq 0 ] || { grep -vE '^[0-9]+ [0-9A-F]{14}([0-9A-F]{14})? (expected|optional)( overlap)?$' "$side" | head -3
  fail "$bad malformed sidecar lines"; }
awk 'NR > 1 && $1 < prev { bad = 1 } { prev = $1 } END { exit bad }' "$side" || fail "sidecar offsets decrease"
[ "$(grep -c ' expected' "$side")" -eq "$(field a expected)" ] || fail "sidecar expected count != summary"
[ "$(grep -c ' expected overlap$' "$side")" -eq 0 ] || fail "an overlapped frame is marked expected"
[ "$(awk 'NR == 1 { print $1 }' "$side")" -eq 120000 ] || fail "first frame not at the 50 ms pad (sample 120000)"
[ "$(field a overlaps)" -gt 0 ] || fail "the sample's first second should have overlaps"
grep -q "^skycue-iqgen: warning: $(field a overlaps) pairs of frames overlap; overlapped frames are marked optional" "$tmp/a.err" ||
  { cat "$tmp/a.err"; fail "no overlap warning"; }
ls "$tmp" | grep -q 'tmp' && { ls "$tmp"; fail "temporary output files left behind"; }

# ---- --skip-seconds: the first second dropped, --duration counts from the next frame ----
expect_exit 0 skip "$iqgen" modes "$modes" --skip-seconds 1 --duration 1 -o "$tmp/s.cu8"
grep -qE ' skipped_seconds=1\.000$' "$tmp/skip.out" || { cat "$tmp/skip.out"; fail "skipped_seconds not in the summary"; }
skip_frames=$(field skip frames)
[ "$skip_frames" -ne "$frames" ] || fail "--skip-seconds 1: still $frames frames"
first_a=$(awk 'NR == 1 { print $2 }' "$side") first_s=$(awk 'NR == 1 { print $2 }' "$tmp/s.frames.txt")
[ "$first_a" != "$first_s" ] || fail "--skip-seconds 1: same first frame $first_a"
# Cross-check on the sample itself (in time order, ties in file order): the
# first frame at least 1 s after the earliest, and the frames in the second after it.
want=$(zcat "$modes" | awk -F, '{ t[NR] = $1; h[NR] = $2; if (NR == 1 || $1 < t0) t0 = $1 }
  END { for (i = 1; i <= NR; i++) if (t[i] - t0 >= 1 && (!s || t[i] < t1)) { t1 = t[i]; s = i }
        for (i = 1; i <= NR; i++) if (t[i] - t0 >= 1 && t[i] - t1 < 1) n++
        print h[s], n }')
[ "$want" = "$first_s $skip_frames" ] || fail "--skip-seconds 1: got '$first_s $skip_frames', the sample says '$want'"
[ "$(awk 'NR == 1 { print $1 }' "$tmp/s.frames.txt")" -eq 120000 ] || fail "--skip-seconds: first kept frame not at the pad"
echo "skip-seconds: frames $frames -> $skip_frames, first frame $first_a -> $first_s"
rm -f "$tmp/s.cu8" "$tmp/s.frames.txt"
expect_exit 2 skip_neg "$iqgen" modes "$modes" --skip-seconds -1 -o "$tmp/s.cu8"

# ---- option caps: non-finite, negative or huge values exit 2 (no overflow) ----
for opt in duration spacing-us pad-ms skip-seconds; do
  case $opt in
    duration) over=86400.5 ;;
    spacing-us) over=1000000001 ;;
    pad-ms) over=60000.5 ;;
    skip-seconds) over=31622401 ;;
  esac
  for v in nan inf -inf -1 "$over" 1e300; do
    expect_exit 2 "cap_${opt}_$v" "$iqgen" modes "$modes" "--$opt=$v" -o "$tmp/cap.cu8"
    grep -q -- "--$opt" "$tmp/cap_${opt}_$v.err" || { cat "$tmp/cap_${opt}_$v.err"; fail "--$opt $v: option not named"; }
    [ -e "$tmp/cap.cu8" ] && fail "--$opt $v wrote an output"
  done
done
for v in nan inf -inf -1 1e300; do
  expect_exit 2 "cap_uat_pad_$v" "$iqgen" uat "$uat" "--pad-ms=$v" -o "$tmp/cap.cu8"
done
# The caps themselves are accepted (--pad-ms 60000 would write 2 min of IQ: not run).
expect_exit 0 cap_ok "$iqgen" modes "$modes" --duration 0.01 --spacing-us 1000000000 -o "$tmp/cap.cu8"
expect_exit 0 cap_ok_duration "$iqgen" modes "$modes" --duration 86400 --skip-seconds 119.8 -o "$tmp/cap.cu8"
expect_exit 2 cap_ok_skip "$iqgen" modes "$modes" --skip-seconds 31622400 -o "$tmp/cap.cu8"
grep -q 'no frames' "$tmp/cap_ok_skip.err" || { cat "$tmp/cap_ok_skip.err"; fail "--skip-seconds 31622400 not accepted"; }
rm -f "$tmp/cap.cu8" "$tmp/cap.frames.txt"

# ---- --skip-seconds on an untimed input: ignored, with a warning ----
zcat "$uat" | grep '^+' | head -3 >"$tmp/untimed.txt"
expect_exit 0 skip_untimed "$iqgen" uat "$tmp/untimed.txt" --skip-seconds 5 -o "$tmp/ut.cu8"
grep -q "^skycue-iqgen: warning: $tmp/untimed.txt: no timestamps; --skip-seconds ignored$" "$tmp/skip_untimed.err" ||
  { cat "$tmp/skip_untimed.err"; fail "--skip-seconds on an untimed input: no warning"; }
grep -qE '^frames=3 .* skipped_seconds=0\.000$' "$tmp/skip_untimed.out" ||
  { cat "$tmp/skip_untimed.out"; fail "--skip-seconds on an untimed input: summary"; }
expect_exit 0 noskip_untimed "$iqgen" uat "$tmp/untimed.txt" -o "$tmp/ut.cu8"
[ -s "$tmp/noskip_untimed.err" ] && { cat "$tmp/noskip_untimed.err"; fail "untimed input without --skip-seconds warned"; }
rm -f "$tmp/ut.cu8" "$tmp/ut.frames.txt" "$tmp/untimed.txt"

# ---- determinism ---------------------------------------------------------------
expect_exit 0 b "$iqgen" modes "$modes" --duration 1 -o "$tmp/b.cu8"
cmp -s "$tmp/a.1090.cu8" "$tmp/b.cu8" || fail "same command, different output"
cmp -s "$side" "$tmp/b.frames.txt" || fail "same command, different sidecar"
expect_exit 0 c "$iqgen" modes "$modes" --duration 1 --seed 2 -o "$tmp/c.cu8"
cmp -s "$tmp/a.1090.cu8" "$tmp/c.cu8" && fail "--seed 2 gave the same output"
[ "$(size "$tmp/c.cu8")" -eq "$bytes" ] || fail "--seed 2 changed the size"
rm -f "$tmp/b.cu8" "$tmp/c.cu8"

# ---- info ------------------------------------------------------------------------
expect_exit 0 info_a "$iqgen" info "$tmp/a.1090.cu8"
grep -qE '^samples=[0-9]+ seconds=[0-9]+\.[0-9]{3} mean_mag=[0-9.]+ peak_mag=[0-9.]+ dc_i=-?[0-9.]+ dc_q=-?[0-9.]+$' \
  "$tmp/info_a.out" || { cat "$tmp/info_a.out"; fail "info line"; }
[ "$(field info_a samples)" -eq $((bytes / 2)) ] || fail "info samples != bytes/2"
awk -v s="$(field info_a seconds)" -v b="$bytes" 'BEGIN { d = s - b / 4.8e6; exit !(d <= 0.001 && d >= -0.001) }' ||
  fail "info seconds $(field info_a seconds) vs bytes/4.8e6"

expect_exit 0 n "$iqgen" modes "$modes" --duration 1 --snr none -o "$tmp/n.cu8"
expect_exit 0 info_n "$iqgen" info "$tmp/n.cu8"
[ "$(field info_n dc_q)" = "0.000" ] || { cat "$tmp/info_n.out"; fail "--snr none: dc_q != 0.000 (pulses are on I only)"; }
awk -v v="$(field info_n dc_i)" 'BEGIN { exit !(v > 0) }' || fail "--snr none: dc_i not > 0"
awk -v v="$(field info_n peak_mag)" 'BEGIN { exit !(v >= 31.5) }' || fail "--snr none: peak_mag < 31.5"
rm -f "$tmp/n.cu8" "$tmp/n.frames.txt"

# UAT without noise is a constant envelope: 12 uplinks back to back (8904
# samples each; 4274 us -> 8904 samples), no pad, so every sample is in a frame
# and there are >= 100 000 of them.
zcat "$uat" | grep '^+' | head -12 >"$tmp/up.txt"
expect_exit 0 up "$iqgen" uat "$tmp/up.txt" --spacing-us 4274 --pad-ms 0 --snr none -o "$tmp/up.cu8"
grep -q '^frames=12 .* overlaps=0 ' "$tmp/up.out" || { cat "$tmp/up.out"; fail "12 adjacent uplinks"; }
expect_exit 0 info_up "$iqgen" info "$tmp/up.cu8" --rate 2083333
[ "$(field info_up samples)" -ge 100000 ] || fail "uplink file has fewer than 100000 samples"
for k in dc_i dc_q; do
  awk -v v="$(field info_up $k)" 'BEGIN { exit !(v <= 1.0 && v >= -1.0) }' || { cat "$tmp/info_up.out"; fail "uat $k not within +-1.0"; }
done
awk -v v="$(field info_up mean_mag)" 'BEGIN { d = v - 32.15; exit !(d <= 1 && d >= -1) }' ||
  { cat "$tmp/info_up.out"; fail "uat mean_mag not within 1 of 32.15"; }
rm -f "$tmp/up.cu8"

# ---- a 5 s slice ---------------------------------------------------------------------
start=$(date +%s%N)
expect_exit 0 five "$iqgen" modes "$modes" --duration 5 -o "$tmp/five.1090.cu8"
elapsed_ms=$((($(date +%s%N) - start) / 1000000))
five_size=$(size "$tmp/five.1090.cu8")
rm -f "$tmp/five.1090.cu8" "$tmp/five.1090.frames.txt"
[ "$elapsed_ms" -lt 30000 ] || fail "modes --duration 5 took ${elapsed_ms} ms (>= 30 s)"
[ "$five_size" -le $((25 * 1024 * 1024)) ] || fail "5 s slice is $five_size bytes (> 25 MB)"

# ---- uat ---------------------------------------------------------------------------------
expect_exit 0 u "$iqgen" uat "$uat" --limit 20 -o "$tmp/u.978.cu8"
grep -qE '^frames=20 expected=20 overlaps=0 clipped=[0-9]+ skipped=0 seconds=[0-9]+\.[0-9]{3} bytes=[0-9]+ skipped_seconds=0\.000$' "$tmp/u.out" ||
  { cat "$tmp/u.out"; fail "uat summary"; }
grep -q 'overlap' "$tmp/u.err" && { cat "$tmp/u.err"; fail "uat: overlap warning without overlaps"; }
us="$tmp/u.978.frames.txt"
[ "$(wc -l <"$us")" -eq 20 ] || fail "uat sidecar lines"
bad=$(grep -cvE '^[0-9]+ [0-9a-f]+ expected$' "$us")
[ "$bad" -eq 0 ] || { head -3 "$us" | cut -c1-80; fail "uat sidecar: $bad lines not lowercase hex + expected"; }
awk 'NR == 1 && $1 != 104167 { exit 1 } NR > 1 && $1 - prev != 41667 { exit 1 } { prev = $1 }' "$us" ||
  { awk '{ print $1 }' "$us" | head -3; fail "uat untimed offsets not 104167 + k * 41667"; }
rm -f "$tmp/u.978.cu8"

# ---- uat: the stratux log, timed by its "<ns>," offsets ---------------------------------
# Its 4 "START,..." session headers are the skipped lines; every payload is 18, 34 or 432 bytes.
# A busy 5 s of session 1, 459 s in: 101 frames (15 overlapping pairs, as recorded).
expect_exit 0 s "$iqgen" uat "$stratux" --skip-seconds 459 --duration 5 -o "$tmp/s.978.cu8"
grep -qE '^frames=101 expected=78 overlaps=15 clipped=0 skipped=4 seconds=5\.[0-9]{3} bytes=[0-9]+ skipped_seconds=459\.000$' \
  "$tmp/s.out" || { cat "$tmp/s.out" "$tmp/s.err"; fail "stratux summary"; }
grep -q 'untimed' "$tmp/s.err" && { cat "$tmp/s.err"; fail "stratux lines placed untimed"; }
rm -f "$tmp/s.978.cu8"
fi  # have_raw
# Sessions in file order, each 1 s after the one before: 5.0/5.2 s, a START, 1.0 s
# (session 2 at 6.2 s), then a backward jump to 0.5 s (session 3 at 7.2 s).
p18=$(printf '0a%.0s' {1..18})
printf '5000000000,-%s;\n5200000000,-%s;\nSTART,x\n1000000000,-%s;\n500000000,-%s;\n' \
  "$p18" "$p18" "$p18" "$p18" >"$tmp/stx_two.log"
expect_exit 0 stratux_sessions "$iqgen" uat "$tmp/stx_two.log" -o "$tmp/stx_two.cu8"
# 50 ms pad, then (t - 5.0 s) * 2083333.3 S/s, rounded.
[ "$(awk '{ printf "%s ", $1 }' "$tmp/stx_two.frames.txt")" = "104167 520834 2604167 4687500 " ] ||
  { cat "$tmp/stx_two.frames.txt" | cut -c1-40; fail "stratux_sessions: not in file order 1 s apart"; }
grep -q ' skipped=1 ' "$tmp/stratux_sessions.out" || { cat "$tmp/stratux_sessions.out"; fail "stratux_sessions: START not counted"; }
rm -f "$tmp/stx_two.cu8"

if [ "$have_raw" = 1 ]; then  # resamples the 1 s slice
# ---- resample ---------------------------------------------------------------------
expect_exit 0 r "$iqgen" resample "$tmp/a.1090.cu8" 2400000 -o "$tmp/r.cu8" --out-rate 2000000
grep -qE '^in_samples=[0-9]+ out_samples=[0-9]+$' "$tmp/r.out" || { cat "$tmp/r.out"; fail "resample line"; }
n=$(field r in_samples) m=$(field r out_samples)
[ "$n" -eq $((bytes / 2)) ] || fail "resample in_samples=$n, want $((bytes / 2))"
d=$((m * 6 - n * 5)); [ "${d#-}" -le 6 ] || fail "resample out_samples=$m, want $n * 5/6 +- 1"
[ "$(size "$tmp/r.cu8")" -eq $((2 * m)) ] || fail "resample output size"
expect_exit 0 r2 bash -c '"$1" resample - 2000000 -o "$2" <"$3"' _ "$iqgen" "$tmp/r2.cu8" "$tmp/r.cu8"
grep -qE "^in_samples=$m out_samples=[0-9]+$" "$tmp/r2.out" || { cat "$tmp/r2.out"; fail "resample from stdin"; }
[ -s "$tmp/r2.cu8" ] || fail "resample from stdin wrote nothing"
expect_exit 2 r_ratio "$iqgen" resample "$tmp/r.cu8" 2083333 -o "$tmp/r3.cu8"
grep -q 2083333 "$tmp/r_ratio.err" && grep -q 2400000 "$tmp/r_ratio.err" || { cat "$tmp/r_ratio.err"; fail "refused ratio: rates not named"; }
[ -e "$tmp/r3.cu8" ] && fail "refused ratio wrote an output"
expect_exit 2 r_zero "$iqgen" resample "$tmp/r.cu8" 0 -o "$tmp/r3.cu8"
rm -f "$tmp/r.cu8" "$tmp/r2.cu8"
fi  # have_raw

# ---- input error paths --------------------------------------------------------------
printf -- '-%s;\n' "$(printf 'ab%.0s' {1..20})" >"$tmp/bad.txt"   # 20 bytes: no UAT length
expect_exit 2 bad_len "$iqgen" uat "$tmp/bad.txt" -o "$tmp/e.cu8"
grep -q 'bad.txt:1' "$tmp/bad_len.err" || { cat "$tmp/bad_len.err"; fail "bad payload length: FILE:LINE not named"; }
: >"$tmp/in.xyz"
expect_exit 2 bad_suffix "$iqgen" modes "$tmp/in.xyz" -o "$tmp/e.cu8"
grep -q 'in.xyz' "$tmp/bad_suffix.err" || { cat "$tmp/bad_suffix.err"; fail "unknown suffix not named"; }
expect_exit 2 missing "$iqgen" modes "$tmp/nope.csv" -o "$tmp/e.cu8"
grep -q 'nope.csv' "$tmp/missing.err" || { cat "$tmp/missing.err"; fail "missing input not named"; }
expect_exit 2 snr "$iqgen" modes "$any" --snr loud -o "$tmp/e.cu8"
grep -q -- '--snr' "$tmp/snr.err" || { cat "$tmp/snr.err"; fail "--snr loud: option not named"; }
[ -e "$tmp/e.cu8" ] && fail "an input error wrote an output"

# ---- size_guard: two frames one hour apart -------------------------------------------
df17=8D4840D6202CC371C32CE0576098
printf '@000000000000%s;\n@000A0EEBB000%s;\n' "$df17" "$df17" >"$tmp/hour.avr"
expect_exit 2 guard "$iqgen" modes "$tmp/hour.avr" -o "$tmp/hour.cu8"
grep -q -- '--max-mb' "$tmp/guard.err" || { cat "$tmp/guard.err"; fail "size guard: --max-mb not named"; }
grep -q -- '--duration' "$tmp/guard.err" || { cat "$tmp/guard.err"; fail "size guard: --duration not suggested"; }
[ -e "$tmp/hour.cu8" ] || [ -e "$tmp/hour.frames.txt" ] && fail "size guard wrote an output"
expect_exit 0 guard_ok "$iqgen" modes "$tmp/hour.avr" --max-mb 100000 --duration 0.01 -o "$tmp/hour.cu8"
grep -q '^frames=1 ' "$tmp/guard_ok.out" || { cat "$tmp/guard_ok.out"; fail "size guard: --duration 0.01 not one frame"; }
rm -f "$tmp/hour.cu8"

# ---- no_frames ---------------------------------------------------------------------------
: >"$tmp/empty.csv"
printf '# only a comment\n# and another\n' >"$tmp/comments.txt"
for f in empty.csv comments.txt; do
  expect_exit 2 "nf_$f" "$iqgen" modes "$tmp/$f" -o "$tmp/nf.cu8"
  grep -qE '^skycue-iqgen: no frames in the input \([0-9]+ lines skipped\)$' "$tmp/nf_$f.err" ||
    { cat "$tmp/nf_$f.err"; fail "$f: no 'no frames' message"; }
  [ -e "$tmp/nf.cu8" ] || [ -e "$tmp/nf.frames.txt" ] && fail "$f: no frames but an output was written"
done
expect_exit 2 nf_uat "$iqgen" uat "$tmp/comments.txt" -o "$tmp/nf.cu8"
grep -q 'no frames in the input' "$tmp/nf_uat.err" || fail "uat comments-only: no 'no frames' message"
# stratux_all_bad_lines: wrong-length stratux payloads are skipped, not fatal; none left -> no frames.
for k in 0 1 2; do printf '%d,-%s;rs=0;\n' $((k * 1000000)) "$(printf 'ab%.0s' {1..20})"; done >"$tmp/stx_bad.log"
expect_exit 2 stratux_all_bad_lines "$iqgen" uat "$tmp/stx_bad.log" -o "$tmp/nf.cu8"
grep -qx 'skycue-iqgen: no frames in the input (3 lines skipped)' "$tmp/stratux_all_bad_lines.err" ||
  { cat "$tmp/stratux_all_bad_lines.err"; fail "stratux_all_bad_lines: not 'no frames ... (3 lines skipped)'"; }
[ -e "$tmp/nf.cu8" ] || [ -e "$tmp/nf.frames.txt" ] && fail "stratux_all_bad_lines: an output was written"
# One good line among them: it is placed, the 3 bad ones counted.
{ cat "$tmp/stx_bad.log"; printf '3000000,-%s;\n' "$(printf '0a%.0s' {1..18})"; } >"$tmp/stx_one.log"
expect_exit 0 stratux_one_good "$iqgen" uat "$tmp/stx_one.log" -o "$tmp/stx_one.cu8"
grep -q '^frames=1 .* skipped=3 ' "$tmp/stratux_one_good.out" || { cat "$tmp/stratux_one_good.out"; fail "stratux_one_good: not frames=1 skipped=3"; }
rm -f "$tmp/stx_one.cu8"

# ---- mixed timed and untimed lines: placed untimed, with a warning ----------------------
printf '@000000000000%s;\n*%s;\n' "$df17" "$df17" >"$tmp/mixed.avr"
expect_exit 0 mixed "$iqgen" modes "$tmp/mixed.avr" -o "$tmp/mixed.cu8"
grep -q 'warning: .*mixed.avr: 1 of 2 lines untimed' "$tmp/mixed.err" || { cat "$tmp/mixed.err"; fail "no untimed warning"; }
[ "$(awk 'NR == 2 { print $1 }' "$tmp/mixed.frames.txt")" -eq $((120000 + 4800)) ] || fail "mixed file not placed untimed"
rm -f "$tmp/mixed.cu8"

if [ "$have_raw" = 1 ]; then  # writes 1 s of the capture
# ---- write failure ----------------------------------------------------------------------
expect_exit 1 full "$iqgen" modes "$modes" --duration 0.1 -o /dev/full
grep -q '^skycue-iqgen: writing /dev/full: ' "$tmp/full.err" || { cat "$tmp/full.err"; fail "/dev/full: error not named"; }
test -c /dev/full || fail "/dev/full is no longer a character device"

# A file limit (SIGXFSZ ignored, so write() fails with EFBIG): nothing is left
# behind, and a file that was already there is untouched.
( trap '' XFSZ; ulimit -f 100; "$iqgen" modes "$modes" --duration 1 -o "$tmp/lim.cu8" ) >"$tmp/xfsz.out" 2>"$tmp/xfsz.err"
rc=$?
[ "$rc" -eq 1 ] || { cat "$tmp/xfsz.err"; fail "file limit: exit $rc, want 1"; }
grep -q '^skycue-iqgen: writing .*lim.cu8: ' "$tmp/xfsz.err" || { cat "$tmp/xfsz.err"; fail "file limit: error not named"; }
ls "$tmp" | grep -q '^lim\.' && { ls "$tmp"; fail "file limit: output or temporary file left behind"; }
echo keep >"$tmp/old.cu8"
( trap '' XFSZ; ulimit -f 100; "$iqgen" modes "$modes" --duration 1 -o "$tmp/old.cu8" ) >"$tmp/xfsz.out" 2>"$tmp/xfsz.err"
[ $? -eq 1 ] || fail "file limit over an existing file: not exit 1"
[ "$(cat "$tmp/old.cu8")" = keep ] || fail "a failed run changed the existing output"
ls "$tmp" | grep -qE '^old\.(frames|cu8\.tmp)' && { ls "$tmp"; fail "file limit: files left next to old.cu8"; }
fi  # have_raw

if [ "$have_raw" = 1 ]; then
  echo "iqgen_cli: all checks passed"
else
  echo "iqgen_cli: all checks on generated inputs passed (test data sections skipped)"
fi
