#!/usr/bin/env bash
# verify-package-inner.sh [systemd]: runs inside a skycue-verify:<target>
# image (packaging/verify-package.sh starts it, with no network) where the
# skycue packages are installed. The source tarball is mounted at
# /src.tar.gz (for the readsb fixture and the parent project).
# Environment: VERIFY_TARGET, VERIFY_VERSION, VERIFY_KIND (deb|rpm),
# VERIFY_DEV (1: a dev package is installed).
# Without an argument: the checks of the packaging spec (section 8.2), in
# order, with the daemon run by hand as the skycue user. "systemd" (in a
# container whose PID 1 is systemd): the unit started, checked, and made to
# fail on a missing receiver position.
# A failure prints "verify: FAIL <target>: <check>" and exits 1; so does an
# unexpected error (the ERR trap, which -E carries into functions).
set -Eeuo pipefail

T=${VERIFY_TARGET:-?}
V=${VERIFY_VERSION:?VERIFY_VERSION not set}
KIND=${VERIFY_KIND:?VERIFY_KIND not set}
DEV=${VERIFY_DEV:-0}
conf=/etc/skycue/skycue.conf
unit=/usr/lib/systemd/system/skycued.service
state=/var/lib/skycue
stats=$state/skycue.stats.json
base=http://127.0.0.1:8080
bg_pids=()

fail() { echo "verify: FAIL $T: $*" >&2; exit 1; }
trap 'fail "unexpected error at line $LINENO: $BASH_COMMAND"' ERR
say() { echo "verify: $T: $*"; }
kill_bg() {
  local p
  for p in "${bg_pids[@]}"; do kill "$p" 2>/dev/null || true; done
  for p in "${bg_pids[@]}"; do wait "$p" 2>/dev/null || true; done
  bg_pids=()
}
trap kill_bg EXIT
# wait_port PORT: up to 10 s for 127.0.0.1:PORT to accept.
wait_port() {
  local i
  for (( i=0; i<100; i++ )); do
    bash -c "exec 3<>/dev/tcp/127.0.0.1/$1" 2>/dev/null && return 0
    sleep 0.1
  done
  return 1
}
# wait_api URL FILE CHECK LIMIT [ARG]: GET URL into FILE every 0.2 s for at
# most LIMIT seconds until CHECK (python, given FILE as sys.argv[1] and ARG as
# sys.argv[2]) prints a line starting "ok". Sets api_out to what CHECK last
# printed and api_err to the last failure.
wait_api() {
  local url=$1 file=$2 check=$3 limit=$4 arg=${5:-} deadline left
  deadline=$((SECONDS + limit))
  api_out= api_err=
  while :; do
    left=$((deadline - SECONDS))
    [ "$left" -gt 0 ] || { api_err="${api_err:+$api_err; }gave up after $limit s"; return 1; }
    if curl -fsS -m "$(( left < 5 ? left : 5 ))" -o "$file" "$url" 2>"$file.curl.err"; then
      if api_out=$(python3 -I -c "$check" "$file" ${arg:+"$arg"} 2>"$file.python.err"); then
        [[ $api_out == ok* ]] && return 0
        api_err="$url: $api_out"
      else
        api_out=error api_err="python: $(tail -n 1 "$file.python.err")"
      fi
    else
      api_out=error api_err="curl: $(tr '\n' ' ' < "$file.curl.err")"
    fi
    sleep 0.2
  done
}
# http_get PATH FILE: GET into FILE; echoes the status (or "curl exit N").
http_get() {
  curl -fsS -m 5 -o "$2" -w '%{http_code}' "$base$1" 2>/dev/null || echo "curl exit $?"
}
# set_key FILE KEY VALUE: replace the one KEY= line of FILE.
set_key() {
  [ "$(grep -c "^$2=" "$1")" = 1 ] || fail "$1 has not exactly one $2= line"
  sed -i "s|^$2=.*|$2=$3|" "$1"
  grep -qx "$2=$3" "$1" || fail "could not set $2 in $1"
}
owner_is_skycue() { [ "$(stat -c %U:%G "$1")" = skycue:skycue ]; }
state_check='import json,sys; n=len(json.load(open(sys.argv[1]))["tracks"])
print("ok %d" % n if n >= int(sys.argv[2] if len(sys.argv) > 2 else 1) else "%d tracks" % n)'

[ -f /src.tar.gz ] || fail "no source tarball mounted at /src.tar.gz"
command -v curl >/dev/null || fail "no curl in the image"
# Under /work, not /tmp: the unit's PrivateTmp would hide /tmp from the daemon.
if [ ! -f /work/CMakeLists.txt ]; then
  mkdir -p /work
  tar -C /work --strip-components=1 -xzf /src.tar.gz || fail "unpacking /src.tar.gz"
  chmod -R a+rX /work
fi
fixture=/work/tests/fixtures/readsb-sample.jsonl
[ -s "$fixture" ] || fail "no readsb fixture in the tarball"

checks() {
  local out rc work copy closed marker stray f d log

  # 1. The version.
  out=$(skycued --version 2>&1) || fail "1 skycued --version exited non-zero: $out"
  [ "$out" = "skycued $V" ] || fail "1 skycued --version printed '$out', want 'skycued $V'"
  say "1 $out"

  # 2. The unit, statically (no systemd running here); deb: enabled, not started.
  [ -f "$unit" ] || fail "2 no $unit"
  out=$(systemd-analyze verify "$unit" 2>&1) || { echo "$out" >&2; fail "2 systemd-analyze verify $unit"; }
  [ -z "$out" ] || echo "$out"
  grep -qx 'ExecStart=/usr/bin/skycued --config /etc/skycue/skycue.conf' "$unit" || fail "2 unit ExecStart"
  grep -qx 'User=skycue' "$unit" || fail "2 unit User"
  grep -qx 'RestartPreventExitStatus=2' "$unit" || fail "2 unit has no RestartPreventExitStatus=2"
  grep -qx 'StateDirectory=skycue' "$unit" || fail "2 unit has no StateDirectory=skycue"
  if [ "$KIND" = deb ]; then
    [ -e /etc/systemd/system/multi-user.target.wants/skycued.service ] ||
      fail "2 skycued.service not enabled (no multi-user.target.wants link)"
  fi
  say "2 systemd-analyze verify $unit"

  # 3. The user, the state directories and the list files.
  getent passwd skycue >/dev/null || fail "3 no skycue user"
  for d in "$state" "$state/maps" "$state/recordings"; do
    [ -d "$d" ] || fail "3 no $d"
    owner_is_skycue "$d" || fail "3 $d is owned by $(stat -c %U:%G "$d"), want skycue:skycue"
  done
  for f in allow.list block.list; do [ -f "$state/$f" ] || fail "3 no $state/$f"; done
  say "3 skycue user, $state with maps and recordings, allow.list and block.list"

  # 4. The config as shipped: one line naming the file and the key, exit 2.
  rc=0; out=$(runuser -u skycue -- skycued --config "$conf" 2>&1) || rc=$?
  [ "$rc" = 2 ] || { echo "$out" >&2; fail "4 shipped config: exit $rc, want 2"; }
  [ "$out" = "skycued: $conf: missing required key RECEIVER_LAT" ] ||
    { echo "$out" >&2; fail "4 shipped config: not the one missing-key line"; }
  say "4 $out"

  # 5. The daemon as the skycue user: packaged config plus a receiver
  # position, the readsb fixture replayed. The copy's directory belongs to
  # skycue, so a file written next to the config would show.
  work=$(mktemp -d /work/run.XXXXXX)
  chown skycue:skycue "$work"
  copy=$work/skycue.conf
  cp "$conf" "$copy"
  set_key "$copy" RECEIVER_LAT 38.9
  set_key "$copy" RECEIVER_LON -77.0
  chown skycue:skycue "$copy"
  log=/work/skycued.log
  marker=/work/run.marker
  touch "$marker"
  sleep 1
  (cd "$work" && exec runuser -u skycue -- skycued --config "$copy" --replay "$fixture" \
     --replay-speed 50 --replay-loop) > "$log" 2>&1 &
  bg_pids+=($!)
  wait_port 8080 || { cat "$log" >&2; fail "5 skycued did not serve on 8080"; }
  out=$(http_get / /work/index.html)
  [ "$out" = 200 ] || fail "5 GET / returned $out"
  grep -q '<title>' /work/index.html || fail "5 GET / has no <title>"
  for f in vendor/leaflet/leaflet.js vendor/echarts/echarts.min.js; do
    out=$(http_get "/$f" /work/asset)
    [ "$out" = 200 ] && [ -s /work/asset ] || fail "5 GET /$f returned $out"
  done
  out=$(http_get /api/tiles-available /work/tiles.json)
  [ "$out" = 200 ] || fail "5 GET /api/tiles-available returned $out"
  python3 -I -c 'import json,sys; sys.exit(json.load(open(sys.argv[1])) != {"tiles": False, "overlays": False})' \
    /work/tiles.json || { cat /work/tiles.json >&2; fail "5 /api/tiles-available is not {\"tiles\":false,\"overlays\":false}"; }
  wait_api "$base/api/state" /work/state.json "$state_check" 30 12 ||
    { tail -n 20 "$log" >&2; echo "verify: last error: $api_err" >&2
      fail "5 GET /api/state: $api_out, want 12 tracks within 30 s"; }
  out=$(http_get /api/stats/summary /work/summary.json)
  [ "$out" = 200 ] || fail "5 GET /api/stats/summary returned $out"
  python3 -I -c 'import json,sys; sys.exit(not isinstance(json.load(open(sys.argv[1])), dict))' /work/summary.json ||
    { head -c 500 /work/summary.json >&2; fail "5 /api/stats/summary is not a JSON object"; }
  kill_bg   # SIGTERM: the daemon saves its stats file on the way out
  [ -f "$stats" ] || { tail -n 20 "$log" >&2; fail "5 no $stats after the run"; }
  owner_is_skycue "$stats" || fail "5 $stats is not owned by skycue"
  [ "$(ls -A "$work")" = skycue.conf ] || { ls -la "$work" >&2; fail "5 the daemon wrote files next to the config"; }
  stray=$(find / -xdev \( -path /proc -o -path /sys -o -path /dev -o -path "$state" \) -prune -o \
            -user skycue -newer "$marker" ! -path "$work" -print 2>/dev/null || true)
  [ -z "$stray" ] || { echo "$stray" >&2; fail "5 the daemon wrote outside $state"; }
  say "5 skycued as skycue: GET /, Leaflet, ECharts, tiles-available (none), /api/state 12 tracks, /api/stats/summary; files under $state only"

  # 6. The map front end, planning only.
  out=$(skycue-make-maps --dry-run --preset conus 2>&1) || { echo "$out" >&2; fail "6 skycue-make-maps --dry-run --preset conus"; }
  say "6 skycue-make-maps --dry-run --preset conus"

  # 7. --check-inputs against a closed port: one line per input, exit 1.
  closed=$(python3 -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
  set_key "$copy" READSB_JSON_PORT "$closed"
  set_key "$copy" DUMP978_RAW_PORT "$closed"
  rc=0
  runuser -u skycue -- skycued --config "$copy" --check-inputs > /work/check.out 2> /work/check.err || rc=$?
  [ "$rc" = 1 ] || { cat /work/check.out /work/check.err >&2; fail "7 --check-inputs: exit $rc, want 1"; }
  [ ! -s /work/check.err ] || { cat /work/check.err >&2; fail "7 --check-inputs wrote to stderr"; }
  [ "$(wc -l < /work/check.out)" = 2 ] || { cat /work/check.out >&2; fail "7 --check-inputs: not exactly two lines"; }
  [ "$(grep -c '^readsb ' /work/check.out)" = 1 ] || { cat /work/check.out >&2; fail "7 --check-inputs: no single readsb line"; }
  [ "$(grep -c '^dump978 ' /work/check.out)" = 1 ] || { cat /work/check.out >&2; fail "7 --check-inputs: no single dump978 line"; }
  ! grep -q 'connected$' /work/check.out || { cat /work/check.out >&2; fail "7 --check-inputs: a closed port says connected"; }
  say "7 --check-inputs on closed port $closed: $(tr '\n' '|' < /work/check.out)"
  rm -rf "$work"

  # 8. A dev package: the parent project against the installed libskycue.
  if [ "$DEV" = 1 ]; then
    cmake -S /work/examples/parent-project -B /tmp/pp -DCLIENT_DEMO_FIND_PACKAGE=ON \
      -DCLIENT_DEMO_WERROR=OFF || fail "8 parent project: configure"
    grep -q '^skycue_DIR:PATH=/usr/lib' /tmp/pp/CMakeCache.txt ||
      { grep '^skycue_DIR' /tmp/pp/CMakeCache.txt >&2; fail "8 parent project did not find the installed skycue"; }
    # Linking client_demo is the check that the packaged libskycue.a is usable
    # by an ordinary (non-LTO) consumer: EL9's rpm flags build it with
    # -flto=auto -ffat-lto-objects.
    cmake --build /tmp/pp -j"$(nproc)" || fail "8 parent project: build (linking the installed libskycue.a)"
    grep -Eq '(^| )/usr/lib[^ ]*/libskycue\.a( |$)' /tmp/pp/CMakeFiles/client_demo.dir/link.txt ||
      { cat /tmp/pp/CMakeFiles/client_demo.dir/link.txt >&2; fail "8 client_demo was not linked against the installed libskycue.a"; }
    out=$(/tmp/pp/client_demo --help 2>&1) || fail "8 client_demo --help exited non-zero"
    [[ $out == usage:* ]] || { echo "$out" >&2; fail "8 client_demo --help does not start with usage:"; }
    say "8 parent project built and linked against the installed libskycue.a (find_package)"
  fi
  say "offline checks passed"
}

# unit_fail WHAT: the FAIL line for a unit that did not do what it should.
# Exit status 226 (NAMESPACE) means systemd could not set up the unit's
# sandbox (ProtectSystem, ProtectHome, PrivateTmp, StateDirectory,
# ReadWritePaths, ReadOnlyPaths). Under rootless podman that is the container's limit, not a
# fault of the unit or of skycued: say so, so that no directive is dropped
# for it.
unit_fail() {
  local st
  journalctl -u skycued --no-pager | tail -n 20 >&2 || true
  st=$(systemctl show -p ExecMainStatus --value skycued 2>/dev/null || true)
  if [ "$st" = 226 ] || journalctl -u skycued --no-pager 2>/dev/null | grep -q '226/NAMESPACE'; then
    fail "systemd: $1: exit 226/NAMESPACE, systemd could not set up the unit's namespaces in this container (a rootless podman limit, not a unit fault; test the hardening with rootful podman or on a real machine)"
  fi
  fail "systemd: $1 (unit fault, exit status ${st:-unknown})"
}

systemd_checks() {
  local i s exec dropin n
  # Wait for systemd to finish booting the container.
  for (( i=0; i<60; i++ )); do
    s=$(systemctl is-system-running 2>/dev/null || true)
    case $s in running|degraded) break ;; esac
    sleep 1
  done
  case $s in running|degraded) ;; *) fail "systemd: not up after 60 s ($s)" ;; esac
  [ ! -e "$stats" ] || fail "systemd: $stats exists before the unit ran"

  # Debian 13's systemd 257 gives journald ImportCredential=journal.*. In a
  # rootless container under qemu-user (the arm64 targets) no unit can set up
  # a credentials directory (exit 243/CREDENTIALS), so journald is dead and
  # journalctl reports "No journal files were found". Start journald without
  # that directive, in this test container only; the check on the journal
  # below stays as strict as before.
  if systemctl is-failed --quiet systemd-journald.service; then
    mkdir -p /etc/systemd/system/systemd-journald.service.d
    printf '[Service]\nImportCredential=\n' > /etc/systemd/system/systemd-journald.service.d/verify-nocred.conf
    systemctl daemon-reload
    systemctl reset-failed systemd-journald.service systemd-journald.socket systemd-journald-dev-log.socket || true
    systemctl start systemd-journald.socket systemd-journald-dev-log.socket systemd-journald.service ||
      fail "systemd: journald does not start (even without ImportCredential)"
    say "systemd: journald had failed (credentials unavailable in this container); restarted without ImportCredential"
  fi

  set_key "$conf" RECEIVER_LAT 38.9
  set_key "$conf" RECEIVER_LON -77.0
  exec=$(sed -n 's/^ExecStart=//p' "$unit")
  [ -n "$exec" ] || fail "systemd: no ExecStart in $unit"
  dropin=/etc/systemd/system/skycued.service.d
  mkdir -p "$dropin"
  printf '[Service]\nExecStart=\nExecStart=%s --replay %s --replay-loop\n' "$exec" "$fixture" > "$dropin/verify-replay.conf"
  systemctl daemon-reload || fail "systemd: daemon-reload"
  systemctl start skycued || unit_fail "systemctl start skycued"
  sleep 5
  s=$(systemctl is-active skycued || true)
  [ "$s" = active ] || unit_fail "skycued is $s after 5 s, want active"
  n=$(systemctl show -p NRestarts --value skycued)
  [ "$n" = 0 ] || fail "systemd: NRestarts=$n, want 0"
  wait_api "$base/api/state" /work/sd-state.json "$state_check" 30 ||
    { journalctl -u skycued --no-pager | tail -n 20 >&2; echo "verify: last error: $api_err" >&2
      fail "systemd: GET /api/state: $api_out, want tracks"; }
  systemctl stop skycued || fail "systemd: systemctl stop skycued"
  [ -f "$stats" ] || { journalctl -u skycued --no-pager | tail -n 20 >&2; fail "systemd: no $stats written under the unit"; }
  owner_is_skycue "$stats" || fail "systemd: $stats is not owned by skycue"
  say "systemd: skycued active (NRestarts=0), /api/state ${api_out#ok } tracks, $stats written"

  # No receiver position: the start fails once and is not retried.
  set_key "$conf" RECEIVER_LAT ''
  set_key "$conf" RECEIVER_LON ''
  systemctl start skycued || true
  for (( i=0; i<30; i++ )); do
    s=$(systemctl is-active skycued || true)
    [ "$s" = failed ] && break
    sleep 0.5
  done
  sleep 3
  s=$(systemctl is-active skycued || true)
  [ "$s" = failed ] || fail "systemd: without a position skycued is $s, want failed"
  n=$(systemctl show -p NRestarts --value skycued)
  [ "$n" = 0 ] || fail "systemd: without a position NRestarts=$n (restart loop), want 0"
  s=$(systemctl show -p ExecMainStatus --value skycued)
  [ "$s" = 2 ] || unit_fail "without a position the exit status is $s, want 2"
  journalctl -u skycued --no-pager | grep -qF "skycued: $conf: missing required key RECEIVER_LAT" ||
    { journalctl -u skycued --no-pager | tail -n 20 >&2; fail "systemd: journal has no missing-key line"; }
  say "systemd: without a position skycued failed (exit 2, not restarted), journal names RECEIVER_LAT"
}

case ${1:-} in
  '') checks ;;
  systemd) systemd_checks ;;
  *) echo "usage: $(basename "$0") [systemd]" >&2; exit 2 ;;
esac
