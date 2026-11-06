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
