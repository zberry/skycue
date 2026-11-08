# skycue-common.sh: shell helpers shared by the skycue scripts. Sourced, not
# run: make-tiles.sh, make-overlays.sh, skycue-make-maps.
# The script that sources it defines die MESSAGE (print and exit 2), which
# load_config calls on a malformed config.

# The GDAL container image the map scripts run when GDAL is not installed
# (make-tiles.sh and make-overlays.sh --container IMAGE, skycue-make-maps
# --container); each takes another image as --container IMAGE.
SKYCUE_GDAL_IMAGE=ghcr.io/osgeo/gdal:ubuntu-small-3.9.3

# Fallback reader for the few keys these scripts need, for machines without
# `skycued` on PATH (a map machine that only builds tiles). Where skycued is
# installed the scripts take the values from `eval "$(skycued --config FILE
# --dump-env)"` instead, which is the only full parser (it also applies the
# overrides file and the SKYCUE_* environment); this reader sees the one file
# and accepts the example's grammar subset only.
# load_config FILE VAR=table.key...: read FILE (skycue.toml: a strict subset of
# TOML, see packaging/skycue.toml.example) for the keys asked for, each
# assigned to its shell variable: load_config "$f" MAP_DIR=paths.map_dir.
# Nothing in the file is expanded or executed. Tables, "basic strings" without
# escapes, 'literal strings', numbers, true/false, arrays (skipped) and #
# comments are read; a malformed line, or a key given twice, calls die with
# the file and line. Keys not asked for are left to skycued, which checks them.
load_config() {
  local file=$1 line n=0 table="" spec var key val seen=" "
  shift
  local re_blank='^[[:space:]]*(#.*)?$'
  local re_table='^[[:space:]]*\[([A-Za-z0-9_]+(\.[A-Za-z0-9_]+)*)\][[:space:]]*(#.*)?$'
  local re_assign='^[[:space:]]*([A-Za-z0-9_]+)[[:space:]]*=[[:space:]]*(.*)$'
  local re_dquote='^"([^"\\]*)"[[:space:]]*(#.*)?$'
  local re_squote="^'([^']*)'[[:space:]]*(#.*)?\$"
  local re_plain='^([A-Za-z0-9_.+-]+)[[:space:]]*(#.*)?$'
  local re_array='^\['
  while IFS= read -r line || [ -n "$line" ]; do
    n=$((n + 1))
    line=${line%$'\r'}
    [[ $line =~ $re_blank ]] && continue
    if [[ $line =~ $re_table ]]; then table=${BASH_REMATCH[1]}; continue; fi
    [[ $line =~ $re_assign ]] || die "$file:$n: malformed line (want [table] or key = value)"
    key=${BASH_REMATCH[1]} val=${BASH_REMATCH[2]}
    [ -n "$table" ] && key=$table.$key
    if [[ $val =~ $re_dquote || $val =~ $re_squote ]]; then
      val=${BASH_REMATCH[1]}
    elif [[ $val =~ $re_plain ]]; then
      val=${BASH_REMATCH[1]}
    elif [[ $val =~ $re_array ]]; then
      val=""
    else
      die "$file:$n: malformed value for $key (a quoted string, a number or true/false; no \$, \` or \\)"
    fi
    [[ $seen == *" $key "* ]] && die "$file:$n: $key is given twice"
    seen+="$key "
    for spec in "$@"; do
      [ "${spec#*=}" = "$key" ] || continue
      var=${spec%%=*}
      printf -v "$var" '%s' "$val"
    done
  done < "$file"
}
