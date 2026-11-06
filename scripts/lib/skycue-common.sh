# skycue-common.sh: shell helpers shared by the skycue scripts. Sourced, not
# run: skycue-start.sh, make-tiles.sh, make-overlays.sh, skycue-make-maps.
# The script that sources it defines die MESSAGE (print and exit 2), which
# load_config calls on a malformed config.

# The GDAL container image the map scripts run when GDAL is not installed
# (make-tiles.sh and make-overlays.sh --container IMAGE, skycue-make-maps
# --container); each takes another image as --container IMAGE.
SKYCUE_GDAL_IMAGE=ghcr.io/osgeo/gdal:ubuntu-small-3.9.3

# load_config FILE [KEY...]: read FILE in the config grammar (see
# packaging/skycue.conf.example): KEY=value, KEY="value" or KEY='value' lines,
# blank lines and # comments. Nothing in the file is expanded or executed;
# a later assignment of a key wins. Without KEYs, every key is assigned to
# the shell variable of its name and must be one of $KNOWN_KEYS (the
# caller's list). With KEYs, only those are assigned and other keys are
# left alone (skycued checks them); every line is still checked.
load_config() {
  local file=$1 line n=0 key val
  shift
  local re_blank='^[[:space:]]*(#.*)?$'
  local re_assign='^([A-Z][A-Z0-9_]*)=(.*)$'
  local re_plain='^[A-Za-z0-9_.,:/+@%=-]*$'
  local re_dquote='^"([^"$`\\]*)"$'
  local re_squote="^'([^']*)'\$"
  local known want=""
  if [ $# -gt 0 ]; then want=" $* "; else known=" ${KNOWN_KEYS//$'\n'/ } "; fi
  while IFS= read -r line || [ -n "$line" ]; do
    n=$((n + 1))
    [[ $line =~ $re_blank ]] && continue
    [[ $line =~ $re_assign ]] || die "$file:$n: malformed line (want KEY=value, KEY=\"value\" or KEY='value')"
    key=${BASH_REMATCH[1]} val=${BASH_REMATCH[2]}
    if [[ $val =~ $re_dquote || $val =~ $re_squote ]]; then
      val=${BASH_REMATCH[1]}
    elif ! [[ $val =~ $re_plain ]]; then
      die "$file:$n: malformed value for $key (quote it; no \$, \`, \\ or nested quotes)"
    fi
    if [ -n "$want" ]; then
      [[ $want == *" $key "* ]] || continue
    else
      [[ $known == *" $key "* ]] || die "unknown key $key in $file"
    fi
    printf -v "$key" '%s' "$val"
  done < "$file"
}
