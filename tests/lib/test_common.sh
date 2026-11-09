# Shared by the shell tests (source it; it is not run on its own).
#
# SKYCUE_TEST_SLOW is the factor the build sets for slow machines (emulated
# architectures, sanitizers; see SKYCUE_TEST_SLOW_FACTOR). Every wait that
# bounds how long a test gives something to happen is multiplied by it, as the
# C++ tests do through test_slow.hpp. At factor 1 the bounds are the ones
# written in the scripts. A wait that returns as soon as its condition holds
# costs nothing extra when the machine is fast. Deliberate pacing (a peer
# that sends slowly on purpose) is not scaled.
slowf=${SKYCUE_TEST_SLOW:-1}

# slow_tenths SECONDS: SECONDS * factor, in 0.1 s polls, rounded up.
slow_tenths() {
  awk -v s="$1" -v f="$slowf" 'BEGIN { n = int(s * 10 + 0.5) * f; i = int(n); if (n > i) i++; print i }'
}

# slow_sleep SECONDS: sleep SECONDS * factor (a "give it time" wait).
slow_sleep() {
  sleep "$(awk -v s="$1" -v f="$slowf" 'BEGIN { print s * f }')"
}

# wait_for SECONDS CMD [ARG...]: poll CMD every 0.1 s until it succeeds
# (return 0) or SECONDS * factor have passed (return 1).
wait_for() {
  local secs=$1 i n
  shift
  n=$(slow_tenths "$secs")
  for (( i=0; i<n; i++ )); do "$@" && return 0; sleep 0.1; done
  return 1
}

_file_nonempty() { [ -s "$1" ]; }
# wait_file FILE SECONDS: until FILE is non-empty.
wait_file() { wait_for "$2" _file_nonempty "$1"; }

# free_port [tcp|udp]: a port on 127.0.0.1 that was free a moment ago (tcp
# when no protocol is given).
free_port() {
  python3 -I -c 'import socket,sys
t = socket.SOCK_DGRAM if sys.argv[1] == "udp" else socket.SOCK_STREAM
s = socket.socket(socket.AF_INET, t); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])' "${1:-tcp}"
}

# test_init NAME: the setup every script test starts with. Sets TEST_NAME, a
# scratch directory $tmp, an empty bg_pids array, and runs cleanup on exit.
test_init() {
  TEST_NAME=$1
  tmp=$(mktemp -d)
  bg_pids=()
  trap cleanup EXIT
}

# bg CMD [ARG...]: run CMD in the background and remember its pid in bg_pids
# (cleanup stops it). $! is set as for a plain `CMD &`.
bg() {
  "$@" &
  bg_pids+=($!)
}

# cleanup: stop every pid in bg_pids with SIGTERM, give each up to 3 s (times
# the slow factor, polled every 0.1 s) to go, SIGKILL what is left, reap, and
# remove $tmp. Safe under set -e. A script that needs more on exit defines its own exit trap and
# calls cleanup from it.
cleanup() {
  local p i
  for p in ${bg_pids[@]+"${bg_pids[@]}"}; do kill "$p" 2>/dev/null || true; done
  for p in ${bg_pids[@]+"${bg_pids[@]}"}; do
    for (( i=0; i<30*slowf; i++ )); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$p" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  [ -n "${tmp:-}" ] && rm -rf "$tmp"
  return 0
}

# fail MSG...: report the failure and exit 1. $current (a step name the script
# keeps up to date) takes the place of the test name when set. A script that
# wants logs shown defines fail_context, which runs after the message.
fail() {
  echo "FAIL (${current:-$TEST_NAME}): $*"
  if declare -F fail_context >/dev/null; then fail_context; fi
  exit 1
}
