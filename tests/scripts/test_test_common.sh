#!/usr/bin/env bash
# The shared shell-test helpers in lib/test_common.sh: test_init, bg, cleanup,
# fail and free_port, each exercised in a subshell of its own.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
common="$here/lib/test_common.sh"
[ -f "$common" ] || { echo "FAIL: test_test_common: $common missing"; exit 1; }
. "$common"
die() { echo "FAIL: test_test_common: $*"; exit 1; }
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# test_init makes $tmp; leaving the shell removes it.
( . "$common"; test_init demo; [ -d "$tmp" ] || exit 1; echo "$tmp" >"$out/tmp1" ) \
  || die "test_init left no directory"
[ -n "$(cat "$out/tmp1")" ] && [ ! -e "$(cat "$out/tmp1")" ] || die "tmp survived the subshell"

# bg registers the pid; cleanup stops it.
( . "$common"; test_init demo; bg sleep 30; echo "$!" >"$out/pid1"
  [ "${bg_pids[0]}" = "$!" ] || exit 1
  kill -0 "$!" 2>/dev/null || exit 2
  cleanup
  kill -0 "$!" 2>/dev/null && exit 3
  exit 0 ); rc=$?
[ "$rc" = 0 ] || die "bg/cleanup on a plain sleep failed (status $rc)"
! kill -0 "$(cat "$out/pid1")" 2>/dev/null || die "bg process still running"

# A process that ignores TERM is killed within 3 * slowf + 2 seconds.
start=$SECONDS
( . "$common"; test_init demo; bg bash -c 'trap "" TERM; sleep 30'; echo "$!" >"$out/pid2"
  sleep 0.3; cleanup; kill -0 "$!" 2>/dev/null && exit 3; exit 0 ); rc=$?
[ "$rc" = 0 ] || die "cleanup did not kill a TERM-ignoring process (status $rc)"
[ $((SECONDS - start)) -le $((3 * slowf + 2)) ] || die "cleanup took $((SECONDS - start)) s"

# fail prints the name (or $current) and exits 1.
got=$( . "$common"; test_init demo; fail boom ); rc=$?
[ "$rc" = 1 ] || die "fail exited $rc"
[ "$got" = "FAIL (demo): boom" ] || die "fail printed '$got'"
got=$( . "$common"; test_init demo; current=step2; fail boom ); rc=$?
[ "$rc" = 1 ] && [ "$got" = "FAIL (step2): boom" ] || die "fail with current printed '$got' (status $rc)"
got=$( . "$common"; test_init demo; fail_context() { echo "log tail"; }; fail boom )
[ "$got" = "$(printf 'FAIL (demo): boom\nlog tail')" ] || die "fail_context output '$got'"

# free_port prints a usable port for both protocols.
for proto in tcp udp; do
  p=$(free_port "$proto")
  case "$p" in ''|*[!0-9]*) die "free_port $proto printed '$p'" ;; esac
  [ "$p" -ge 1024 ] && [ "$p" -le 65535 ] || die "free_port $proto out of range: $p"
done
echo "PASS: test_test_common"
