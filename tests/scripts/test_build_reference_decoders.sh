#!/usr/bin/env bash
# scripts/dev/build-reference-decoders.sh: argument handling, the printed pins and the
# digest check. Never builds anything: the digest case feeds a wrong tarball offline.
# Usage: test_build_reference_decoders.sh SCRIPT
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib/test_common.sh"
script=$1
test_init build_reference_decoders
[ -f "$script" ] || fail "$script missing"

current=no-argument
rc=0
bash "$script" >"$tmp/o" 2>"$tmp/e" || rc=$?
[ "$rc" -eq 2 ] || fail "exit $rc, want 2"
grep -q 'usage:' "$tmp/e" || fail "stderr lacks usage:"

current=help
rc=0
bash "$script" --help >"$tmp/o" 2>"$tmp/e" || rc=$?
[ "$rc" -eq 0 ] || fail "exit $rc, want 0"
for s in v3.16.21.tar.gz f40aa9a0d2d0a0067c0c628b44ba316f7049b8ce.tar.gz \
  b89c0b9a241384b709a9febfea61533169f4e7b3be5f01b99810e36b751c0a03 \
  c2f4acd912c6117dbd17e44dc380df6e4acc8cfa5e071d426988b447fadc205a; do
  grep -q -- "$s" "$tmp/o" || fail "--help lacks $s"
done

current=digest-mismatch
mkdir -p "$tmp/src"
echo wrong | gzip >"$tmp/src/readsb.tar.gz"
rc=0
SKYCUE_REFERENCE_OFFLINE=1 SKYCUE_REFERENCE_SRC_DIR="$tmp/src" \
  bash "$script" "$tmp/out" >"$tmp/o" 2>"$tmp/e" || rc=$?
[ "$rc" -eq 1 ] || fail "exit $rc, want 1"
grep -q 'digest mismatch' "$tmp/e" || fail "stderr lacks 'digest mismatch'"
[ ! -e "$tmp/out/bin" ] || fail "bin directory exists after a failed check"
echo "PASS (build_reference_decoders)"
