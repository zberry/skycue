#!/usr/bin/env bash
# What `cmake --install` puts where, for the configuration of this build tree:
# binaries, the web UI with its vendored libraries and their licences, the
# headers and the CMake package config always; with SKYCUE_INSTALL_SYSTEM_FILES
# also the map scripts with their front bin/skycue-make-maps (which must find
# them and run them, --dry-run, on the map dir it is given), the config (the
# example, or with SKYCUE_PACKAGE_CONFIG the packaged skycue.toml) and the
# skycued unit, whose ExecStart names this build's own bindir and sysconfdir;
# without it none of those (a parent build's layout). Nothing is installed outside the prefix except the config
# and the unit, and no map tiles or overlays are installed at all.
# The install goes to a DESTDIR, since the config and the unit may have
# absolute destinations (/etc, /usr/lib/systemd/system).
# usage: test_install_layout.sh CMAKE BUILD_DIR WORK_DIR PREFIX BINDIR SYSCONFDIR
#          SYSTEM_FILES(ON|OFF) PACKAGE_CONFIG(ON|OFF) UNITDIR
#   BINDIR, SYSCONFDIR: the full (absolute) directories; UNITDIR as cached,
#   relative to PREFIX or absolute.
set -u
cmake=$1 build=$2 work=$3 prefix=$4 bindir=$5 sysconfdir=$6 system_files=$7 package_config=$8 unitdir=$9
fail() { echo "FAIL: $*"; exit 1; }
is_on() { case "${1^^}" in ON|YES|TRUE|Y|1) return 0 ;; *) return 1 ;; esac; }

root=$work/root
rm -rf "$work"
mkdir -p "$work" || fail "cannot create $work"
DESTDIR=$root "$cmake" --install "$build" >"$work/install.log" 2>&1 \
  || { tail -n 20 "$work/install.log"; fail "cmake --install failed"; }
p=$root$prefix
[ -d "$p" ] || fail "nothing installed under $prefix"

need() { for f in "$@"; do [ -f "$p/$f" ] || fail "not installed: $prefix/$f"; done; }
need bin/skycued bin/skycue-dump \
  share/skycue/www/index.html \
  share/skycue/www/vendor/leaflet/leaflet.js share/skycue/www/vendor/leaflet/LICENSE \
  share/skycue/www/vendor/echarts/echarts.min.js share/skycue/www/vendor/echarts/LICENSE \
  share/skycue/www/vendor/echarts/NOTICE share/skycue/www/vendor/echarts/licenses/LICENSE-d3 \
  include/skycue/client.hpp include/skycue/version.hpp
for f in bin/skycued bin/skycue-dump; do [ -x "$p/$f" ] || fail "$prefix/$f is not executable"; done
configs=$(cd "$p" && find lib* -path '*/cmake/skycue/skycueConfig.cmake' 2>/dev/null)
[ "$(printf '%s\n' "$configs" | grep -c .)" = 1 ] \
  || fail "want one lib*/.../cmake/skycue/skycueConfig.cmake, got: ${configs:-none}"
for d in tiles overlays; do
  [ -e "$p/share/skycue/www/$d" ] && fail "$prefix/share/skycue/www/$d was installed"
done

case $unitdir in /*) unit=$unitdir/skycued.service ;; *) unit=$prefix/$unitdir/skycued.service ;; esac
if is_on "$package_config"; then
  conf=$sysconfdir/skycue/skycue.toml other=$sysconfdir/skycue/skycue.toml.example
else
  conf=$sysconfdir/skycue/skycue.toml.example other=$sysconfdir/skycue/skycue.toml
fi
scripts=share/skycue/scripts

if is_on "$system_files"; then
  need $scripts/make-tiles.sh $scripts/make-overlays.sh $scripts/tiles-merge.py \
    $scripts/lib/skycue-common.sh bin/skycue-make-maps
  for f in $scripts/make-tiles.sh $scripts/make-overlays.sh $scripts/tiles-merge.py bin/skycue-make-maps; do
    [ -x "$p/$f" ] || fail "$prefix/$f is not executable"
  done
  # The installed front finds the installed helpers and gives them the map
  # dir and the cache (their own defaults would point into share/skycue/www).
  mm=$("$p/bin/skycue-make-maps" --map-dir "$work/maps" --cache "$work/cache" --preset world \
         --container img:1 --dry-run 2>&1) || fail "installed skycue-make-maps --dry-run: $mm"
  grep -qxF "would add to $work/maps/tiles/meta.json: {\"name\":\"global\",\"bbox\":[-180,-85,180,85],\"minZoom\":0,\"maxZoom\":5}" <<<"$mm" \
    || fail "installed skycue-make-maps: no global region for $work/maps/tiles: $mm"
  grep -qF "would write: $work/maps/overlays/admin0.json" <<<"$mm" \
    || fail "installed skycue-make-maps: overlays not for $work/maps/overlays: $mm"
  grep -q 'share/skycue/www' <<<"$mm" && fail "installed skycue-make-maps: a helper default leaked in: $mm"
  [ -e "$work/maps" ] && fail "installed skycue-make-maps --dry-run created the map dir"
  [ -f "$root$conf" ] || fail "not installed: $conf"
  [ -e "$root$other" ] && fail "installed $other as well as $conf"
  grep -Eq '^lat = 0\.0( |$)' "$root$conf" || fail "$conf has no lat = 0.0"
  if is_on "$package_config"; then
    grep -qx 'state_dir = "/var/lib/skycue"' "$root$conf" || fail "$conf is not the packaged config"
  else
    cmp -s "$root$conf" "$(dirname "$0")/../packaging/skycue.toml.example" \
      || fail "$conf is not the example as it is in etc/"
  fi
  [ -f "$root$unit" ] || fail "not installed: $unit"
  u=$root$unit
  grep -q '@' "$u" && fail "$unit has an unexpanded @VAR@: $(grep '@' "$u")"
  exec_want="ExecStart=$bindir/skycued --config $sysconfdir/skycue/skycue.toml"
  [ "$(grep '^ExecStart=' "$u")" = "$exec_want" ] \
    || fail "$unit: $(grep '^ExecStart=' "$u"), want $exec_want"
  case $bindir$sysconfdir in *//*|*/./*|*/../*) fail "unnormalised path in $exec_want" ;; esac
  for line in '[Unit]' '[Service]' '[Install]' Type=simple User=skycue Group=skycue \
              After=network-online.target Wants=network-online.target Restart=on-failure \
              RestartPreventExitStatus=2 ProtectSystem=strict StateDirectory=skycue ReadWritePaths=/var/lib/skycue \
              ReadOnlyPaths=-/var/lib/skycue/maps \
              ProtectHome=yes PrivateTmp=yes NoNewPrivileges=yes WantedBy=multi-user.target; do
    [ "$(grep -cxF -- "$line" "$u")" = 1 ] || fail "$unit: want exactly one line $line"
  done
  grep -q '^Requires=' "$u" && fail "$unit requires another unit"
  allowed_outside=("$conf" "$unit")
else
  for f in $scripts bin/skycue-make-maps; do
    [ -e "$p/$f" ] && fail "SKYCUE_INSTALL_SYSTEM_FILES=OFF installed $prefix/$f"
  done
  for f in "$conf" "$other" "$unit"; do
    [ -e "$root$f" ] && fail "SKYCUE_INSTALL_SYSTEM_FILES=OFF installed $f"
  done
  [ -n "$(cd "$root" && find . -name 'skycued.service' -o -name 'skycue.toml*')" ] \
    && fail "SKYCUE_INSTALL_SYSTEM_FILES=OFF installed a unit or config"
  allowed_outside=()
fi

# Every installed file is under the prefix, or is the config or the unit.
while IFS= read -r f; do
  f=${f#.}
  case $f in "$prefix"/*) continue ;; esac
  ok=0
  for a in "${allowed_outside[@]}"; do [ "$f" = "$a" ] && ok=1; done
  [ "$ok" = 1 ] || fail "installed outside $prefix: $f"
done < <(cd "$root" && find . \( -type f -o -type l \) -print)

rm -rf "$work"
echo "PASS"
