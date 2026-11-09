#!/usr/bin/env bash
# skycued's command line before any configuration is read: --help byte for byte,
# --version, the usage errors (status 2, one "skycued: " line on stderr), the
# --dump-env output, an unreadable config file, and a clean start with SIGTERM.
# usage: test_skycued_cli.sh SKYCUED FIXTURE_DIR VERSION
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib/test_common.sh"
skycued=$1 fixtures=$2 version=$3
test_init skycued_cli
current=setup
for f in "$skycued" "$fixtures/skycued-help.txt"; do [ -e "$f" ] || fail "$f missing"; done

for v in $(compgen -e | grep '^SKYCUE_'); do unset "$v"; done

help_is_the_fixture() {
  "$skycued" --help >"$tmp/help" 2>"$tmp/err" || fail "--help exit $?"
  cmp -s "$tmp/help" "$fixtures/skycued-help.txt" || fail "--help differs: $(diff "$tmp/help" "$fixtures/skycued-help.txt" | head -5)"
  [ ! -s "$tmp/err" ] || fail "--help wrote to stderr"
  "$skycued" -h >"$tmp/h" 2>/dev/null || fail "-h exit $?"
  cmp -s "$tmp/h" "$tmp/help" || fail "-h differs from --help"
}

version_prints_the_project_version() {
  out=$("$skycued" --version 2>"$tmp/err") || fail "--version exit $?"
  [ "$out" = "skycued $version" ] || fail "--version printed '$out'"
  [ ! -s "$tmp/err" ] || fail "--version wrote to stderr"
}

# expect_usage MESSAGE ARG...: status 2, empty stdout, stderr exactly MESSAGE.
expect_usage() {
  local msg=$1
  shift
  "$skycued" "$@" >"$tmp/out" 2>"$tmp/err"; rc=$?
  [ "$rc" = 2 ] || fail "$*: exit $rc"
  [ ! -s "$tmp/out" ] || fail "$*: stdout not empty"
  [ "$(cat "$tmp/err")" = "$msg" ] || fail "$*: stderr '$(cat "$tmp/err")'"
}

usage_errors_exit_2() {
  expect_usage "skycued: --log-level must be debug, info, warn or error, got 'nope'" --log-level nope
  expect_usage "skycued: --config is required"
  expect_usage "skycued: --cue-tcp-port must be 0..65535" --config x --cue-tcp-port 70000
  expect_usage "skycued: --web-port must be 0..65535" --config x --web-port 70000
  expect_usage "skycued: --web-bind must not be empty" --config x --web-bind ''
  expect_usage "skycued: --readsb-host must not be empty" --config x --readsb-host ''
  expect_usage "skycued: --dump978-host must not be empty" --config x --dump978-host ''
  expect_usage "skycued: --check-inputs cannot be combined with --replay" --config x --check-inputs --replay y
  expect_usage "skycued: --replay-speed must be 0 (unpaced) or positive" --config x --replay y --replay-speed -1
}

unknown_flag_shows_the_table() {
  "$skycued" --bogus-flag >"$tmp/out" 2>"$tmp/err"; rc=$?
  [ "$rc" = 2 ] || fail "exit $rc"
  [ ! -s "$tmp/out" ] || fail "stdout not empty"
  head -n 1 "$tmp/err" | grep -q '^skycued: ' || fail "first stderr line: $(head -n 1 "$tmp/err")"
  grep -q -- '--config' "$tmp/err" || fail "the option table is not shown"
}

missing_config_exits_2() {
  "$skycued" --config "$tmp/absent.toml" >"$tmp/out" 2>"$tmp/err"; rc=$?
  [ "$rc" = 2 ] || fail "exit $rc"
  [ ! -s "$tmp/out" ] || fail "stdout not empty"
  [ "$(grep -c '' "$tmp/err")" = 1 ] && grep -q '^skycued: ' "$tmp/err" || fail "stderr '$(cat "$tmp/err")'"
}

dump_env_prints_assignments() {
  printf '[receiver]\nlat = 38.9\nlon = -77.0\n' >"$tmp/ok.toml"
  out=$("$skycued" --config "$tmp/ok.toml" --dump-env 2>"$tmp/err") || fail "exit $?"
  [ "$(grep -c '' <<<"$out")" -gt 40 ] || fail "fewer than 41 lines"
  grep -qx "SKYCUE_RECEIVER_LAT='38.9'" <<<"$out" || fail "no SKYCUE_RECEIVER_LAT line"
}

convert_conf_exits_0() {
  printf 'RECEIVER_LAT=38.9\nRECEIVER_LON=-77.0\n' >"$tmp/old.conf"
  "$skycued" --convert-conf "$tmp/old.conf" >"$tmp/out" 2>"$tmp/err"; rc=$?
  [ "$rc" = 0 ] || fail "exit $rc: $(cat "$tmp/err")"
  grep -q '^\[' "$tmp/out" || fail "no TOML table in the output"
  "$skycued" --convert-conf "$tmp/none.conf" >"$tmp/out" 2>"$tmp/err"; rc=$?
  [ "$rc" != 0 ] || fail "a missing file converted"
}

check_inputs_exit_codes() {
  local port
  port=$(free_port)
  printf '[receiver]\nlat = 38.9\nlon = -77.0\n[inputs.readsb]\nhost = "127.0.0.1"\n[inputs.dump978]\nenable = false\n' >"$tmp/c.toml"
  "$skycued" --config "$tmp/c.toml" --check-inputs --set "inputs.readsb.json_port=$port" >"$tmp/out" 2>"$tmp/err"; rc=$?
  [ "$rc" = 1 ] || fail "nothing listening: exit $rc"
  [ "$(grep -c '' "$tmp/out")" -ge 1 ] || fail "no line per input"
  "$skycued" --config "$tmp/c.toml" --check-inputs --no-readsb --no-uat --inputs-file "" >"$tmp/out" 2>"$tmp/err"; rc=$?
  [ "$rc" = 0 ] || fail "no inputs: exit $rc"
  [ "$(cat "$tmp/out")" = "no inputs to check" ] || fail "no inputs: '$(cat "$tmp/out")'"
  "$skycued" --config "$tmp/absent.toml" --check-inputs >/dev/null 2>&1; rc=$?
  [ "$rc" = 2 ] || fail "bad config: exit $rc"
}

clean_start_and_sigterm_exit_0() {
  printf '[receiver]\nlat = 38.9\nlon = -77.0\n[web]\nport = 0\n[cues]\ntcp_port = 0\n[inputs.readsb]\nenable = false\n[inputs.dump978]\nenable = false\n' >"$tmp/s.toml"
  : >"$tmp/log"
  bg "$skycued" --config "$tmp/s.toml" --inputs-file "" >"$tmp/log" 2>&1
  local pid=$!
  wait_for 10 grep -q 'forwarding' "$tmp/log" || fail "never started: $(cat "$tmp/log")"
  kill -TERM "$pid"
  wait "$pid"; rc=$?
  bg_pids=()
  [ "$rc" = 0 ] || fail "SIGTERM: exit $rc"
}

for t in help_is_the_fixture version_prints_the_project_version usage_errors_exit_2 unknown_flag_shows_the_table \
  missing_config_exits_2 dump_env_prints_assignments convert_conf_exits_0 check_inputs_exit_codes \
  clean_start_and_sigterm_exit_0; do
  current=$t
  "$t"
  echo "ok $t"
done
echo "PASS"
