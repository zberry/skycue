#!/bin/bash
# scripts/test-data/gen-icao-tables.py --check on broken copies of www/app_pure.js: a
# renamed country is stale (1); an overlap, a missing table and a bad row are
# errors (2); strings and comments after the tables are not read (0).
# usage: test_icao_generator.sh PYTHON3 GEN_ICAO_TABLES_PY APP_PURE_JS ICAO_TABLES_INC WORK_DIR
set -euo pipefail

py=$1
gen=$2
app=$3
inc=$4
work=$5

mkdir -p "$work"
tmp=$(mktemp -d "$work/icao-generator.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

failed=0
# expect NAME WANT_STATUS FILE
expect() {
    local name=$1 want=$2 file=$3 got=0
    "$py" -I "$gen" --check "$file" "$inc" 2>"$tmp/$name.err" || got=$?
    if [ "$got" -eq "$want" ]; then
        echo "ok   $name (exit $got)"
    else
        echo "FAIL $name: exit $got, want $want; stderr: $(cat "$tmp/$name.err")"
        failed=1
    fi
}
# variant NAME SED_EXPR: a copy of app_pure.js through sed; fails when sed
# changed nothing (the pattern no longer matches the page).
variant() {
    sed "$2" "$app" >"$tmp/$1.js"
    if cmp -s "$app" "$tmp/$1.js"; then
        echo "FAIL $1: the sed expression changed nothing"
        failed=1
    fi
}

expect unchanged 0 "$app"

variant renamed "s/'Zimbabwe'/'Zimbabwe2'/"
expect renamed 1 "$tmp/renamed.js"

variant overlap "s/\[0x006000, 0x006FFF, 'Mozambique'\]/[0x004000, 0x006FFF, 'Mozambique']/"
expect overlap 2 "$tmp/overlap.js"

variant missing "s/var MILITARY_BLOCKS =/var MIL_BLOCKS =/"
expect missing_table 2 "$tmp/missing.js"

variant badrow "s/\[0x010070, 0x01008F\]/[0x010070, 0x01008G]/"
expect bad_row 2 "$tmp/badrow.js"

# Text after the tables the scan must never read: a double-quoted string
# with an apostrophe, a /* */ comment holding ' and ], a regex with '.
cp "$app" "$tmp/after.js"
cat >>"$tmp/after.js" <<'EOF'
var q = "it's"; /* don't ] ; */ var r = /'/;
EOF
expect text_after_tables 0 "$tmp/after.js"

# Inside a table: a /* */ comment with an apostrophe.
variant inner_comment "s|\[0x010070, 0x01008F\], // Egypt|[0x010070, 0x01008F], /* Egypt's */|"
expect comment_in_table 0 "$tmp/inner_comment.js"

[ "$failed" -eq 0 ] && echo PASS
exit "$failed"
