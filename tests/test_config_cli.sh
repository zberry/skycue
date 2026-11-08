#!/usr/bin/env bash
# skycued's configuration command line: --dump-env (shell round trip, errors),
# --set against the overrides file and the environment, --convert-conf of the
# old skycue.conf (the shipped example, an old overrides file, the comment
# count), and the legacy flags applying last.
# usage: test_config_cli.sh SKYCUED EXAMPLE_TOML OLD_EXAMPLE_CONF FIXTURE_DIR
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib/test_common.sh"
skycued=$1 example=$2 old_example=$3 fixtures=$4
tmp=$(mktemp -d)
bg_pids=()
cleanup() {
  local p i
  for p in "${bg_pids[@]}"; do kill "$p" 2>/dev/null; done
  for p in "${bg_pids[@]}"; do
    for (( i=0; i<30*slowf; i++ )); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$p" 2>/dev/null
  done
  wait 2>/dev/null
  rm -rf "$tmp"
}
trap cleanup EXIT
fail() { echo "FAIL ($current): $*"; exit 1; }
current=setup
for f in "$skycued" "$example" "$old_example" "$fixtures/readsb-sample.jsonl"; do [ -e "$f" ] || fail "$f missing"; done

# The tests start from no SKYCUE_* variables of the caller's.
for v in $(compgen -e | grep '^SKYCUE_'); do unset "$v"; done

base() { printf '[receiver]\nlat = 38.9\nlon = -77.0\n'; }
value() { # value NAME < dump-env output: the shell value of NAME as eval sees it
  local out
  out=$(cat)
  ( eval "$out"; printf '%s' "${!1-}" )
}

dump_env_shell_roundtrip() {
  { base; printf '[inputs.readsb]\nhost = "it'"'"'s $HOME x"\n[cues]\nudp_targets = ["a:1", "b:2"]\n'; } >"$tmp/r.toml"
  out=$("$skycued" --config "$tmp/r.toml" --dump-env) || fail "exit $?"
  # eval gives the value back exactly: a quote, a space, a dollar sign.
  got=$( eval "$out"; printf '%s' "$SKYCUE_INPUTS_READSB_HOST" )
  [ "$got" = "it's \$HOME x" ] || fail "host came back as '$got'"
  got=$( eval "$out"; printf '%s' "$SKYCUE_CUES_UDP_TARGETS" )
  [ "$got" = "a:1 b:2" ] || fail "udp_targets came back as '$got'"
  got=$( eval "$out"; printf '%s' "$SKYCUE_INPUTS_DUMP978_ENABLE-$SKYCUE_RECEIVER_LAT" )
  [ "$got" = "true-38.9" ] || fail "enable/lat came back as '$got'"
  # Plain assignments, one per key: no export, nothing but SKYCUE_ names.
  grep -q 'export' <<<"$out" && fail "export in the output"
  [ "$(grep -c '' <<<"$out")" -eq "$(grep -cE "^SKYCUE_[A-Z0-9_]+='([^']|'\\\\'')*'\$" <<<"$out")" ] ||
    fail "a line is not NAME='value'"
  [ "$(grep -c '' <<<"$out")" -ge 40 ] || fail "fewer than 40 keys"
  # Evaluating them does not export them.
  [ -z "$( eval "$out"; printenv SKYCUE_INPUTS_READSB_HOST )" ] || fail "exported"
}

dump_env_error_exit2_empty_stdout() {
  printf '[receiver]\nlat = 0.0\nlon = 0.0\n' >"$tmp/zero.toml"
  printf '[receiver]\nlat = 38.9\nlon = -77.0\nbogus = 1\n' >"$tmp/bogus.toml"
  for case in "zero.toml" "bogus.toml" "missing.toml"; do
    "$skycued" --config "$tmp/$case" --dump-env >"$tmp/out" 2>"$tmp/err"; rc=$?
    [ "$rc" = 2 ] || fail "$case: exit $rc"
    [ ! -s "$tmp/out" ] || fail "$case: stdout not empty"
    [ -s "$tmp/err" ] || fail "$case: no message"
  done
  base >"$tmp/ok.toml"
  "$skycued" --config "$tmp/ok.toml" --set nope=1 --dump-env >"$tmp/out" 2>"$tmp/err"; rc=$?
  [ "$rc" = 2 ] && [ ! -s "$tmp/out" ] && grep -q nope "$tmp/err" || fail "--set unknown key: $rc"
  "$skycued" --config "$tmp/ok.toml" --set web.port=notanumber --dump-env >"$tmp/out" 2>"$tmp/err"; rc=$?
  [ "$rc" = 2 ] && [ ! -s "$tmp/out" ] || fail "--set bad value: $rc"
}

set_overrides_file_value() {
  mkdir "$tmp/so"
  { base; printf '[filters]\nmax_range_nm = 100\n'; } >"$tmp/so/skycue.toml"
  printf '[filters]\nmax_range_nm = 90\n' >"$tmp/so/skycue.overrides.toml"
  c="$tmp/so/skycue.toml"
  range() { "$skycued" --config "$c" "$@" --dump-env | value SKYCUE_FILTERS_MAX_RANGE_NM; }
  [ "$(range)" = 90 ] || fail "overrides file over the main file: $(range)"
  [ "$(range --set filters.max_range_nm=70)" = 70 ] || fail "--set over the overrides file"
  [ "$(SKYCUE_FILTERS_MAX_RANGE_NM=80 range)" = 80 ] || fail "environment over the overrides file"
  [ "$(SKYCUE_FILTERS_MAX_RANGE_NM=80 range --set filters.max_range_nm=70)" = 70 ] || fail "--set over the environment"
  [ "$(range --set filters.max_range_nm=60 --set filters.max_range_nm=65)" = 65 ] || fail "later --set wins"
  # A bare word is a string; an array needs its brackets.
  [ "$("$skycued" --config "$c" --set inputs.readsb.host=10.0.0.2 --dump-env | value SKYCUE_INPUTS_READSB_HOST)" = 10.0.0.2 ] ||
    fail "bare-word --set"
}

no_uat_flag_wins_over_set() {
  base >"$tmp/f.toml"
  v() { "$skycued" --config "$tmp/f.toml" "$@" --dump-env | value "$NAME"; }
  NAME=SKYCUE_INPUTS_DUMP978_ENABLE
  [ "$(v --set inputs.dump978.enable=true --no-uat)" = false ] || fail "--no-uat under --set"
  [ "$(SKYCUE_INPUTS_DUMP978_ENABLE=true v --no-uat)" = false ] || fail "--no-uat under the environment"
  NAME=SKYCUE_INPUTS_READSB_ENABLE
  [ "$(v --set inputs.readsb.enable=true --no-readsb)" = false ] || fail "--no-readsb under --set"
  NAME=SKYCUE_INPUTS_READSB_HOST
  [ "$(v --set inputs.readsb.host=10.0.0.2 --readsb-host 10.0.0.3)" = 10.0.0.3 ] || fail "--readsb-host under --set"
  NAME=SKYCUE_INPUTS_DUMP978_HOST
  [ "$(v --set inputs.dump978.host=10.0.0.2 --dump978-host 10.0.0.4)" = 10.0.0.4 ] || fail "--dump978-host under --set"
  # The startup lines name the source of each such key.
  "$skycued" --config "$tmp/f.toml" --replay "$fixtures/readsb-sample.jsonl" --replay-speed 0 \
    --set web.port=0 --set cues.tcp_port=0 --set inputs.dump978.enable=true --no-uat >"$tmp/start.log" 2>&1 &
  bg_pids+=($!)
  wait_file "$tmp/start.log" 10 && wait_for 10 grep -q ' forwarding' "$tmp/start.log" || { cat "$tmp/start.log"; fail "no startup"; }
  grep -q 'config: inputs.dump978.enable = false (flag)' "$tmp/start.log" || { cat "$tmp/start.log"; fail "no flag line"; }
  grep -q 'config: web.port = 0 (--set)' "$tmp/start.log" || { cat "$tmp/start.log"; fail "no --set line"; }
  grep -q 'inputs.dump978.enable = true (--set)' "$tmp/start.log" && fail "a --set line for a key a flag replaced"
  grep -q 'STATE_DIR\|DUMP978_ENABLE' "$tmp/start.log" && fail "old names in the startup output"
  "$skycued" --help | grep -q 'inputs.dump978.enable' || fail "--no-uat help names the old key"
}

with_lat_lon() { sed -e 's/^RECEIVER_LAT=$/RECEIVER_LAT=51.5/' -e 's/^RECEIVER_LON=$/RECEIVER_LON=-0.12/' "$1"; }

convert_conf_example_matches_toml_example() {
  with_lat_lon "$old_example" >"$tmp/old.conf"
  "$skycued" --convert-conf "$tmp/old.conf" >"$tmp/new.toml" 2>/dev/null || fail "convert exit $?"
  "$skycued" --config "$tmp/new.toml" --dump-env >"$tmp/a.env" 2>"$tmp/a.err" || { cat "$tmp/a.err"; fail "converted file rejected"; }
  "$skycued" --config "$example" --set receiver.lat=51.5 --set receiver.lon=-0.12 --dump-env >"$tmp/b.env" ||
    fail "example rejected"
  diff "$tmp/a.env" "$tmp/b.env" || fail "converted example differs from the TOML example"
  # Values do convert: yes/no, a space-separated list, quoted values, ports.
  cat >"$tmp/vals.conf" <<'CONF'
RECEIVER_LAT=38.9
RECEIVER_LON="-77.0"
DUMP978_ENABLE=no
RECORD_RAW=yes
RECORD_DIR='/data/a # b $c'
RECORD_FORMATS=jsonl
CUE_UDP_TARGETS="10.0.0.1:5000 10.0.0.2:5001"
WEB_PORT=8081
MAX_RANGE_NM=90
MAX_RANGE_NM=120
GAIN_1090=49.6
CONF
  "$skycued" --convert-conf "$tmp/vals.conf" >"$tmp/vals.toml" 2>/dev/null || fail "convert vals exit $?"
  out=$("$skycued" --config "$tmp/vals.toml" --dump-env) || fail "vals rejected"
  chk() { [ "$( eval "$out"; printf '%s' "${!1}" )" = "$2" ] || fail "$1 is not '$2'"; }
  chk SKYCUE_INPUTS_DUMP978_ENABLE false
  chk SKYCUE_RECORDING_RAW true
  chk SKYCUE_RECORDING_DIR '/data/a # b $c'
  chk SKYCUE_RECORDING_FORMATS jsonl
  chk SKYCUE_CUES_UDP_TARGETS '10.0.0.1:5000 10.0.0.2:5001'
  chk SKYCUE_WEB_PORT 8081
  chk SKYCUE_FILTERS_MAX_RANGE_NM 120
  chk SKYCUE_SDR_GAIN_1090 49.6
}

convert_conf_overrides_style() {
  printf '# saved\nMAX_RANGE_NM=90\nBLOCK_LIST_FILE=block.txt\n' >"$tmp/o.conf"
  "$skycued" --convert-conf "$tmp/o.conf" >"$tmp/o.toml" 2>/dev/null || fail "exit $?"
  [ "$(grep -c '' "$tmp/o.toml")" -le 8 ] || fail "not an overrides-style file"
  grep -q '^\[filters\]$' "$tmp/o.toml" && grep -qx 'max_range_nm = 90' "$tmp/o.toml" &&
    grep -qx 'block_list_file = "block.txt"' "$tmp/o.toml" || { cat "$tmp/o.toml"; fail "keys missing"; }
  grep -q 'receiver' "$tmp/o.toml" && fail "main-file keys in an overrides file"
  # It is a valid overrides file: it sits beside a main file and takes effect.
  mkdir "$tmp/ov"
  base >"$tmp/ov/skycue.toml"
  cp "$tmp/o.toml" "$tmp/ov/skycue.overrides.toml"
  [ "$("$skycued" --config "$tmp/ov/skycue.toml" --dump-env | value SKYCUE_FILTERS_MAX_RANGE_NM)" = 90 ] ||
    fail "overrides output not read as an overrides file"
}

convert_conf_reports_dropped_comments() {
  with_lat_lon "$old_example" >"$tmp/old.conf"
  want=$(grep -cE '^[[:space:]]*#' "$tmp/old.conf")
  "$skycued" --convert-conf "$tmp/old.conf" >/dev/null 2>"$tmp/err" || fail "exit $?"
  grep -qE "(^|[^0-9])$want comment lines" "$tmp/err" || { cat "$tmp/err"; fail "stderr does not say $want comment lines"; }
  # The example's own comments are in the output.
  "$skycued" --convert-conf "$tmp/old.conf" 2>/dev/null | grep -q 'ground altitude, metres above mean sea level' ||
    fail "the example's comments are missing"
  printf 'RECEIVER_LAT=1\nRECEIVER_LON=2\n' >"$tmp/nocomment.conf"
  "$skycued" --convert-conf "$tmp/nocomment.conf" >/dev/null 2>"$tmp/err" || fail "exit $?"
  [ ! -s "$tmp/err" ] || fail "a note without comments"
  # Errors: exit 2, a message naming the line, nothing on stdout.
  for bad in 'FOO=1' 'RECEIVER_LAT=$HOME' 'not a line' 'DUMP978_ENABLE=maybe' 'WEB_PORT=abc'; do
    printf 'RECEIVER_LAT=1\nRECEIVER_LON=2\n%s\n' "$bad" >"$tmp/bad.conf"
    "$skycued" --convert-conf "$tmp/bad.conf" >"$tmp/out" 2>"$tmp/err"; rc=$?
    [ "$rc" = 2 ] && [ ! -s "$tmp/out" ] && grep -q 'bad.conf:3' "$tmp/err" || { cat "$tmp/err"; fail "'$bad': exit $rc"; }
  done
  "$skycued" --convert-conf "$tmp/missing.conf" >/dev/null 2>&1; [ $? = 2 ] || fail "missing file"
}

for t in dump_env_shell_roundtrip dump_env_error_exit2_empty_stdout set_overrides_file_value \
         no_uat_flag_wins_over_set convert_conf_example_matches_toml_example convert_conf_overrides_style \
         convert_conf_reports_dropped_comments; do
  current=$t
  "$t"
  echo "ok $t"
done
echo "PASS"
