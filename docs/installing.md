# Installing skycue

Updated 2026-11-04, for skycue 0.1.0.

skycue runs beside the decoders you already have: it reads readsb's JSON
position output (and, in the USA, dump978-fa's UAT stream) and never feeds,
replaces, restarts or configures them. This page takes you from nothing to
the map in the browser:

1. install skycue (a package, or a build from source),
2. tell it where your receiver is,
3. turn on the one decoder output it reads, and check that skycue can
   reach it,
4. start it,
5. bake the offline map,
6. open the page.

The commands below set `v` to the release you install; change that line
when you install a later one. Every other line can be pasted as it is.

## Debian and Raspberry Pi

Debian 12 and 13 and Raspberry Pi OS (64-bit), on amd64 and arm64. Each
release has a build per Debian release, told apart by the end of the
version: pick `skycue_<version>-1~deb12u1_<arch>.deb` on Debian 12 and on
Raspberry Pi OS (based on Debian 12, bookworm), and
`skycue_<version>-1~deb13u1_<arch>.deb` on Debian 13. The two are not
interchangeable (each needs its own release's Boost libraries). Put the
one you need in the current directory.

### 1. Install the package

```sh
v=0.1.0
d=$(. /etc/os-release && echo "$VERSION_ID")   # 12 (also Raspberry Pi OS) or 13
sudo apt install ./skycue_${v}-1~deb${d}u1_$(dpkg --print-architecture).deb
```

This installs `skycued`, the `skycue-*` tools, the web page, the config
`/etc/skycue/skycue.conf` and the systemd unit `skycued.service` (in
`/lib/systemd/system` on Debian 12, `/usr/lib/systemd/system` on Debian 13;
on a merged-/usr system both name the same directory). It creates the
system user `skycue` and the state directory `/var/lib/skycue` (with `maps/`
and `recordings/`), and enables the unit without starting it: the config
needs your receiver's position first. It installs no decoder and changes nothing of theirs.

### 2. Edit the config

```sh
sudoedit /etc/skycue/skycue.conf
```

Set at least these lines (the values here are examples):

```ini
# Your antenna, in decimal degrees (required).
RECEIVER_LAT=51.4700
RECEIVER_LON=-0.4543
# The machine readsb runs on: 127.0.0.1 when it is this one.
READSB_HOST=127.0.0.1
# no when there is no 978 MHz (UAT) receiver, which is everyone outside the USA.
DUMP978_ENABLE=no
```

`RECEIVER_ALT_M` and `ANTENNA_HEIGHT_M` sharpen the elevation angles;
`MAX_RANGE_NM` and `MIN_ELEVATION_DEG` set what is forwarded. Every key is
described in the file and in [operating.md](operating.md#config-keys-that-matter).
skycued keeps its own files (inputs, statistics, the filters you change in
the page) in `/var/lib/skycue` and never writes to `/etc/skycue/skycue.conf`.

### 3. Turn on the decoder output skycue reads

skycue reads one thing from readsb: its JSON position output on TCP port
30047, with aircraft that have no position included. For UAT it also
connects to dump978-fa's raw port, 30978. Pick the block that matches how
your decoders run. Each one ends with the same check.

#### readsb as a system service

readsb installed as a service reads its options from `/etc/default/readsb`.
Add the two JSON options to the end of `NET_OPTIONS` and restart readsb
(run this once):

```sh
sudo sed -i 's/^NET_OPTIONS="\(.*\)"$/NET_OPTIONS="\1 --net-json-port 30047 --net-json-port-include-noposition"/' /etc/default/readsb
grep '^NET_OPTIONS=' /etc/default/readsb
sudo systemctl restart readsb
```

With a 978 MHz receiver and dump978-fa installed as a service: dump978-fa's
`/etc/default/dump978-fa` already serves the raw port in its default
`NET_OPTIONS` (`--raw-port 30978 --json-port 30979`); check it is still
there. readsb then has to read that port too, so UAT aircraft reach skycue
through readsb. If `NET_OPTIONS` in `/etc/default/readsb` has no `uat_in`
connector yet, add one and restart readsb (the `sed` changes nothing when
the line already has a `uat_in` connector, so running it twice is safe):

```sh
grep -o -- '--raw-port [0-9]*' /etc/default/dump978-fa
sudo sed -i '/^NET_OPTIONS=.*uat_in/!s/^NET_OPTIONS="\(.*\)"$/NET_OPTIONS="\1 --net-connector 127.0.0.1,30978,uat_in"/' /etc/default/readsb
sudo systemctl restart readsb
```

and keep `DUMP978_ENABLE=yes`. Without a 978 MHz receiver set
`DUMP978_ENABLE=no` in `/etc/skycue/skycue.conf`.

Check:

```sh
skycued --config /etc/skycue/skycue.conf --check-inputs
```

#### readsb inside ultrafeeder (Docker)

ultrafeeder passes extra options to its readsb through `READSB_EXTRA_ARGS`
and serves the JSON position output on the container's port 30047. Add the
two options and publish the port in your `docker-compose.yml` (merge them
into the `environment:` and `ports:` lists you already have; if
`READSB_EXTRA_ARGS` is already set, append the two options to it):

```yaml
services:
  ultrafeeder:
    environment:
      - READSB_EXTRA_ARGS=--net-json-port 30047 --net-json-port-include-noposition
    ports:
      - 30047:30047
```

```sh
docker compose up -d
```

Set `READSB_HOST` to the machine Docker runs on: `127.0.0.1` when skycue
runs there too (and then `127.0.0.1:30047:30047` in `ports:` keeps the port
off the network), else that machine's address.

With a dump978 container feeding ultrafeeder (`adsb,dump978,30978,uat_in`
in `ULTRAFEEDER_CONFIG`, or `dump978,30978,uat_in` in
`READSB_NET_CONNECTOR`), UAT aircraft already reach skycue through
ultrafeeder's readsb. Publish the dump978 container's raw port as well, so
skycue can count and decode the uplinks, and set `DUMP978_HOST` to the
Docker machine like `READSB_HOST`:

```yaml
services:
  dump978:
    ports:
      - 30978:30978
```

```sh
docker compose up -d
```

Without a dump978 container set `DUMP978_ENABLE=no`.

Check:

```sh
skycued --config /etc/skycue/skycue.conf --check-inputs
```

#### PiAware (dump1090-fa)

dump1090-fa has no JSON position output, so skycue reads it through a
second, network-only readsb that takes dump1090-fa's Beast output (port
30005) and serves the JSON on 30047. That readsb opens no SDR and listens
on no other port, so PiAware's own decoding and feeding carry on untouched.

readsb is not part of PiAware. Build it from its source; a network-only
instance needs no SDR libraries (skycue is tested with readsb 3.16.21):

```sh
sudo apt install build-essential git pkg-config libncurses-dev zlib1g-dev libzstd-dev
git clone --depth 1 --branch v3.16.21 https://github.com/wiedehopf/readsb.git
make -C readsb -j"$(nproc)"
sudo install -m 755 readsb/readsb /usr/local/bin/readsb
```

Run it as a service of its own, `readsb-skycue`:

```sh
sudo tee /etc/systemd/system/readsb-skycue.service >/dev/null <<'EOF'
[Unit]
Description=network-only readsb: dump1090-fa Beast to JSON for skycue
After=network-online.target dump1090-fa.service
Wants=network-online.target

[Service]
DynamicUser=yes
ExecStart=/usr/local/bin/readsb --quiet --net-only --net-connector 127.0.0.1,30005,beast_in --net-json-port 30047 --net-json-port-include-noposition
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now readsb-skycue
```

The UAT side: with a 978 MHz receiver, PiAware's dump978-fa (the one
SkyAware978 reads) already serves the raw port 30978, which skycue reads
directly for the uplinks. To see the UAT aircraft on skycue's map as well,
add `--net-connector 127.0.0.1,30978,uat_in` to the `ExecStart=` line of
`readsb-skycue` above (then `sudo systemctl daemon-reload` and
`sudo systemctl restart readsb-skycue`) and keep `DUMP978_ENABLE=yes`.
Without one set `DUMP978_ENABLE=no`. Do not edit `/etc/default/dump978-fa`
on a PiAware image: PiAware rewrites it at boot from its own settings.

Check:

```sh
skycued --config /etc/skycue/skycue.conf --check-inputs
```

#### skycue's own decoder stack (the stack repository)

The stack repository ([below](#the-offline-bundle)) builds readsb
and dump978-fa itself and `skycue-start.sh` starts them with the options
skycue needs; nothing to change. While it runs, from the install prefix:

```sh
cd /opt/skycue
bin/skycued --config etc/skycue/skycue.conf --check-inputs
```

#### What the check prints

One line per input. The exit status is 1 when any input it tried did not
connect, 2 for an inputs file that is too large or not valid (an inputs
file it cannot open counts as absent: only the built-in readsb and dump978
inputs are checked), and 0 otherwise: every input connected, or there was
nothing to try (it prints `no inputs to check`, or `<id>: not checked
(listens on ...)` for an input that listens instead of connecting):

```
readsb 127.0.0.1:30047: connected
dump978 127.0.0.1:30978: connected
```

A line with an error instead of `connected` (`Connection refused`, a name
that does not resolve, a timeout) names the host and port skycue tried: fix
the decoder block or `READSB_HOST` / `DUMP978_HOST`, or set
`DUMP978_ENABLE=no` when there is no UAT receiver, and run the check again.
With `DUMP978_ENABLE=no` there is no `dump978` line.

### 4. Start skycue

The package has already enabled the unit, so it starts at every boot from
now on; start it now:

```sh
sudo systemctl start skycued
systemctl status skycued
```

`journalctl -u skycued` shows its log. A config error (an empty
`RECEIVER_LAT`, say) stops it at once with one line naming the file and the
key; fix the file and `sudo systemctl restart skycued`.

### 5. Bake the map

The map is drawn offline from Natural Earth (public domain). One script
downloads it (about 310 MB, once, cached under `/var/cache/skycue`) and
bakes tiles and overlays into `/var/lib/skycue/maps`. It needs GDAL, and
Pillow for `tiles-merge.py`, which merges the tiles where two regions
overlap (the package recommends curl, unzip, python3 and Pillow, so apt
has usually installed them already):

```sh
sudo apt install gdal-bin python3-gdal python3-pil curl unzip
```

Then bake the region around your receiver, for example 250 nm from the
position in the config:

```sh
sudo skycue-make-maps --around 250
```

Every run first makes sure a `global` region exists (the whole world at
zooms 0 to 5) and then adds the region you asked for at finer zooms, so
runs add up: bake a wider area or a second region later without losing the
first. `--around` does not wrap across the antimeridian or past ±85°
latitude: the box is cut at 180° and at the poles (near 180° longitude, bake
the other side as a second `--bbox` region; see
[make-tiles.md](../scripts/make-tiles.md)). The other ways to name a region:

```sh
sudo skycue-make-maps --preset conus     # also: europe, world
sudo skycue-make-maps --around 51.47 -0.45 100
sudo skycue-make-maps --list             # the regions baked so far
```

`--bbox WEST,SOUTH,EAST,NORTH --zoom MIN-MAX` bakes any box, and
`--dry-run` prints what would run. Without GDAL, `--container` runs the GDAL
steps in a pinned GDAL container image through podman or docker instead
(see [RHEL](#rhel)). The page picks the maps up on its next load; skycued
needs no restart, and it works without maps too (a dark background with a
grid).

### 6. Open the page

On the receiver itself: <http://127.0.0.1:8080/>. To open it from another
machine, see [Opening the page to the LAN](#opening-the-page-to-the-lan)
(or forward the port over ssh, `ssh -L 8080:127.0.0.1:8080` to the
receiver, and open the same address). What the page shows is in
[operating.md](operating.md#the-web-ui).

## RHEL

RHEL 8 and 9 and their rebuilds (Rocky Linux, AlmaLinux), on x86_64. The
package is `skycue-<version>-1.el8.x86_64.rpm` or `.el9.`; put it in the
current directory.

```sh
v=0.1.0
sudo dnf install ./skycue-${v}-1.el$(rpm -E %rhel).x86_64.rpm
```

It installs the same files in the same places as the Debian package
(`/etc/skycue/skycue.conf`, `/var/lib/skycue`, the `skycue` user, the unit)
but leaves the unit disabled, as the distribution's preset does for new
services. Follow the Debian steps
[2. Edit the config](#2-edit-the-config) and
[3. Turn on the decoder output](#3-turn-on-the-decoder-output-skycue-reads):
the decoder blocks are the same wherever readsb and dump978-fa run (on RHEL
usually built from source or in Docker: add the two readsb options to
however yours is started). Then enable and start skycued in one step:

```sh
sudo systemctl enable --now skycued
systemctl status skycued
```

The log and the config errors are as in
[4. Start skycue](#4-start-skycue).

### Bake the map

GDAL is not in RHEL's own repositories, so the map script runs the GDAL
steps in a container: the pinned GDAL image, through podman (the first run
pulls the image and needs the network, as the Natural Earth download does).
Pillow (`python3-pillow`) is for `tiles-merge.py`, which merges the tiles
where two baked regions share tiles (the same tile at the same zoom). On
some EL releases python3-pillow is only in EPEL; the rpm only recommends it,
so the package installs either way. The merge runs on this machine even
with `--container`, so without Pillow keep to regions that do not overlap
(the `global` base and one `--around` region never do: their zooms differ),
or bake on another machine as described below:

```sh
sudo dnf install podman unzip python3
sudo dnf install python3-pillow    # where it is only in EPEL, enable EPEL first or skip it
sudo skycue-make-maps --container --around 250
```

(`curl`, which downloads the Natural Earth data, is part of the base
system.)

`--preset`, `--bbox` and `--list` work as in
[5. Bake the map](#5-bake-the-map). If the machine already uses EPEL, its
`gdal` and `python3-gdal` packages do the same without the container when
they are GDAL 3.1 or newer (`gdalinfo --version`); EPEL is not required.

No container either? Bake on another machine (any Linux with GDAL or
podman, using the source tarball's `scripts/skycue-make-maps`, no install
needed) and copy the result into `/var/lib/skycue/maps`. On the other
machine, with this machine's `skycue.conf` copied next to the unpacked
source tarball:

```sh
v=0.1.0
tar -xzf skycue-${v}.tar.gz
skycue-${v}/scripts/skycue-make-maps --config skycue.conf --map-dir maps --owner "$(id -un)" --around 250
tar -C maps -czf skycue-maps.tar.gz .
```

then, with `skycue-maps.tar.gz` copied here:

```sh
sudo tar -C /var/lib/skycue/maps -xzf skycue-maps.tar.gz
sudo chown -R skycue:skycue /var/lib/skycue/maps
```

Then open the page as in [6. Open the page](#6-open-the-page).

## From source

The same platforms, built with plain CMake from the release's source
tarball `skycue-<version>.tar.gz`, which carries the two web libraries the
page needs (Leaflet and ECharts) so nothing is downloaded at build time.

The build packages, on Debian and Raspberry Pi OS:

```sh
sudo apt install build-essential cmake libboost-system-dev libboost-program-options-dev libboost-test-dev zlib1g-dev python3 curl
```

on RHEL 8 and 9 (BaseOS and AppStream only):

```sh
sudo dnf install gcc-c++ make cmake boost-devel zlib-devel python3 diffutils findutils tar gzip
```

skycue needs CMake 3.25 or newer. On RHEL 8 that is the cmake from a
current minor release (a RHEL 8 compatible build got 3.26.5);
older RHEL 8 minor releases shipped a cmake that is too old, so update
the system first (`sudo dnf update`) if `cmake --version` says less.

Build, test and install (to `/usr/local`):

```sh
v=0.1.0
tar -xzf skycue-${v}.tar.gz
cd skycue-${v}
cmake -S skycue -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build
sudo cmake --install build
```

`curl` is for one of the tests. The headless-browser test skips itself
without node and Playwright, and the tests that need the third-party
captures the source tarball leaves out skip themselves too. This
installs the programs in `/usr/local/bin`, the page in
`/usr/local/share/skycue/www`, the example config
`/usr/local/etc/skycue/skycue.conf.example`, and the unit in
`/usr/local/lib/systemd/system` (which systemd also searches), running
`/usr/local/bin/skycued --config /usr/local/etc/skycue/skycue.conf`.

A package would create the user and the state directory; here they are
made by hand, and the config gets the same four paths the packaged one has:

```sh
sudo useradd --system --user-group --home-dir /var/lib/skycue --no-create-home --shell /usr/sbin/nologin skycue
sudo install -d -o skycue -g skycue -m 0755 /var/lib/skycue /var/lib/skycue/maps /var/lib/skycue/recordings
sudo install -o skycue -g skycue -m 0644 /dev/null /var/lib/skycue/allow.list
sudo install -o skycue -g skycue -m 0644 /dev/null /var/lib/skycue/block.list
sudo cp /usr/local/etc/skycue/skycue.conf.example /usr/local/etc/skycue/skycue.conf
sudo sed -i -e 's|^STATE_DIR=$|STATE_DIR=/var/lib/skycue|' -e 's|^MAP_DIR=$|MAP_DIR=/var/lib/skycue/maps|' -e 's|^ALLOW_LIST_FILE=$|ALLOW_LIST_FILE=allow.list|' -e 's|^BLOCK_LIST_FILE=$|BLOCK_LIST_FILE=block.list|' /usr/local/etc/skycue/skycue.conf
grep -E '^(STATE_DIR|MAP_DIR|ALLOW_LIST_FILE|BLOCK_LIST_FILE)=' /usr/local/etc/skycue/skycue.conf
sudo systemctl daemon-reload
```

Then the same steps as for a package, with the config at
`/usr/local/etc/skycue/skycue.conf`: set the receiver and the decoder keys
as in [2. Edit the config](#2-edit-the-config) (`sudoedit
/usr/local/etc/skycue/skycue.conf`), turn on the decoder output
([3.](#3-turn-on-the-decoder-output-skycue-reads)), then:

```sh
skycued --config /usr/local/etc/skycue/skycue.conf --check-inputs
sudo systemctl enable --now skycued
sudo skycue-make-maps --config /usr/local/etc/skycue/skycue.conf --around 250
```

On RHEL, sudo's `secure_path` leaves out `/usr/local/bin`, so name the
script in full and add `--container`, as in [RHEL](#rhel):

```sh
sudo /usr/local/bin/skycue-make-maps --config /usr/local/etc/skycue/skycue.conf --container --around 250
```

Then open <http://127.0.0.1:8080/>.

## The offline bundle

A full-stack offline bundle that also builds the SDR decoders lives in a separate repository (the stack repository). For a machine without network access it carries every source tarball, builds the SDR libraries, readsb, dump978-fa and skycue into one relocatable prefix, and starts them together with `skycue-start.sh`. This repository builds and installs skycue alone, as described above.

## Opening the page to the LAN

skycued serves the page on `127.0.0.1:8080` only, so by default only the
receiver itself can open it. To open it from other machines, listen on
every interface and restart skycued:

```sh
sudo sed -i 's/^WEB_BIND=.*/WEB_BIND=0.0.0.0/' /etc/skycue/skycue.conf
sudo systemctl restart skycued
```

(from source: `/usr/local/etc/skycue/skycue.conf`). Then open
`http://<receiver address>:8080/`. With firewalld (the RHEL default), also
open the port:

```sh
sudo firewall-cmd --permanent --add-port=8080/tcp
sudo firewall-cmd --reload
```

There is no login: anyone who can reach the port can see the traffic and
change the filters and the allow and block lists, which change what is
forwarded to your cue clients. Open it only to a network you trust. `WEB_PORT`
moves the page to another port, and `WEB_PORT=0` turns it off.
