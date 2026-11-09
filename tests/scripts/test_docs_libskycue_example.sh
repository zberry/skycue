#!/usr/bin/env bash
# docs/libskycue.md's complete example is examples/minimal_consumer.cpp, verbatim.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
fail() { echo "FAIL: test_docs_libskycue_example: $*"; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
awk '
  !seen && $0 == "## A complete minimal example" { seen = 1; next }
  seen && !inb && $0 == "```cpp" { inb = 1; next }
  inb && $0 == "```" { exit }
  inb { print }
' "$here/docs/libskycue.md" > "$tmp/extract.cpp"
[ -s "$tmp/extract.cpp" ] || fail "no cpp block after '## A complete minimal example' in docs/libskycue.md"
diff -u "$tmp/extract.cpp" "$here/examples/minimal_consumer.cpp" > "$tmp/diff" ||
  { cat "$tmp/diff"; fail "docs/libskycue.md's example differs from examples/minimal_consumer.cpp"; }
echo "PASS: test_docs_libskycue_example"
