# Offline map tiles: make-tiles.sh

The web UI (see `docs/operating.md`, "The web UI") works without tiles: it
draws a plain dark background with a 1°/5° graticule, the range and
elevation rings and a scale bar. Tiles add a coastline, terrain shading and
land cover underneath. They are optional, never fetched at run time, and
not part of the repository (`www/tiles/` is git-ignored).

## Making them (connected machine)

```sh
scripts/make-tiles.sh -10,35,30,60 0 8               # west,south,east,north, zoom 0..8
scripts/make-tiles.sh -10,35,30,60 0 8 --dry-run     # print the commands only
scripts/make-tiles.sh -5,50,2,55 0 10 --out /tmp/t   # another output directory
scripts/make-tiles.sh -10,35,30,60 0 8 --container ghcr.io/osgeo/gdal:ubuntu-small-3.9.3   # GDAL in podman/docker
```

`make-tiles.sh BBOX MINZ MAXZ [--name NAME] [--out DIR] [--cache DIR] [--container IMAGE] [--palette] [--meta-only] [--dry-run]`:

1. Downloads the Natural Earth 1:10m "Cross Blended Hypsometric Tints with
   Shaded Relief, Water and Drainages" raster (`NE1_HR_LC_SR_W_DR.zip`,
   about 310 MB, public domain, from the Natural Earth S3 bucket) into the
   cache (default `build/naturalearth/`, git-ignored), once; set
   `SKYCUE_NE_URL` to use a mirror.
2. Crops it to BBOX with `gdal_translate -projwin`.
3. Cuts XYZ tiles with `gdal2tiles.py --xyz -z MINZ-MAXZ` into a staging
   directory in the cache (`stage-NAME`).
4. Merges them into the output directory (default `www/tiles/`),
   laid out as `tiles/{z}/{x}/{y}.png`, which is what the page requests,
   with `scripts/tiles-merge.py`: a tile the directory does not have yet is
   moved in; a tile it already has keeps its pixels where they are opaque
   and gets the new tile's pixels where it is transparent, so the edge
   tiles of neighbouring regions join up and nothing from an earlier run is
   lost.
5. Adds the region to `tiles/meta.json` (`--name`, default `default`):

   ```json
   {"minZoom":0,"maxZoom":9,"bbox":[-180,-85,180,85],
    "regions":[{"name":"global","bbox":[-180,-85,180,85],"minZoom":0,"maxZoom":5},
               {"name":"northeast","bbox":[-76,38.5,-66,46],"minZoom":0,"maxZoom":9}],
    "source":"Natural Earth 1:10m","generated":"2026-11-02T12:00:00Z"}
   ```

   The top-level `minZoom`/`maxZoom` are the overall range and `bbox` the
   union, so a reader that knows only the single-region form keeps working.
   Running again with the same `--name`, or with the same bbox and zoom
   range (under any name), replaces that region's entry in `meta.json`
   only: the tiles already on disk keep their opaque pixels (step 4), so
   re-cutting a region with other settings changes nothing where its old
   tiles were. To really re-cut, delete the directory and run every region
   again. A `meta.json` in the old single-region form (no `regions`) is read
   as one region named `default`; region entries that are not well formed
   are dropped with a warning, and a `meta.json` that is not a JSON object
   stops the run before any tile is cut.
   skycued passes the top-level `minZoom`/`maxZoom` on in
   `/api/tiles-available`; the map limits zooming out to `minZoom`, asks
   for tiles up to `maxZoom` and scales them up beyond it. Where a region
   has no tile at the zoom shown (outside every region with that zoom), the
   page shows the nearest lower-zoom tile it has, enlarged, instead of a
   blank square; it looks at most 4 zoom levels down. A tile on a region's
   edge (not wholly inside the bbox of a region with its zoom) gets its
   nearest ancestor drawn underneath, so its transparent part shows the
   coarser map. So bake a coarse region (such as the whole world, zoom
   0–5) within 4 levels of every finer one: without it, places outside the
   fine regions stay blank at their zooms.

`--meta-only` only does step 5, for tiles that are already there (no GDAL
needed, only `python3`); `--dry-run` prints the commands and the region.
`--palette` writes the new (and composited) tiles as 256-colour PNGs with
Pillow: about a seventh of the size of gdal2tiles' RGBA tiles, with a
mean colour error of about 1%, hardly visible on this raster. Older Pillow
releases work too (the method constant moved to `Image.Quantize` in 9.1).

Several regions with different zoom ranges, e.g. the whole world coarse and
the area around the receiver in detail (about 28 MB in all with
`--palette`, about 200 MB without):

```sh
scripts/make-tiles.sh -180,-85,180,85 0 5 --name global --palette
scripts/make-tiles.sh -125,24,-66,50 6 8 --name conus --palette
scripts/make-tiles.sh -170,51,-129,72 6 8 --name alaska --palette
scripts/make-tiles.sh -161,18.5,-154.5,22.5 6 9 --name hawaii --palette
```

It needs GDAL with its Python tools (`gdal2tiles.py`, `gdal_translate`;
GDAL 3.1 or newer for `--xyz`), `python3`, `curl` and `unzip`, and Pillow
(`python3-pillow`) when tiles of two regions meet or with `--palette` (the
`ghcr.io/osgeo/gdal` container images have all of them). Without `gdal2tiles.py`
it exits with status 3 and says which package to install
(`gdal` and `python3-gdal` with dnf and EPEL, `gdal-bin` and `python3-gdal`
with apt, or `gdal` from conda-forge) or which image to run instead. Usage
errors exit 2.

`--container IMAGE` runs `gdal_translate` and `gdal2tiles.py` in a GDAL
container image instead (`ghcr.io/osgeo/gdal:ubuntu-small-3.9.3` is the one
`skycue-make-maps` and the hints name, set once in
`scripts/lib/skycue-common.sh`), with podman or else docker, exactly as
`make-overlays.sh --container` runs `ogr2ogr`: `<engine> run --rm
--network=none` with the cache bind-mounted at `/c` (`:Z`, relabelled for
SELinux) as the working directory, as the calling user (`--userns=keep-id`
for rootless podman, `--user UID:GID` otherwise), so the raster, the crop and
the staging directory are all in the cache. The download, the merge and
`meta.json` stay on the host (`curl`, `unzip`, `python3`, and Pillow for
compositing as above). With `--dry-run` no engine is needed; without one, a
real run exits 3.

Pick a bbox a little larger than `MAX_RANGE_NM` around the receiver
(1° of latitude is 60 nm). Running the script again with the same output
directory adds to the tiles already there and to `meta.json`; delete the
directory first for a clean set (or to re-cut a region: existing pixels
win, see step 5).

## Size and zoom

The 1:10m raster is about 1 km per pixel; past zoom 8 the tiles are only
enlargements of it, so higher zooms add size without detail. The map scales
the top zoom level up when zooming in further, so there is little reason to
go beyond 8.

| Zoom range | Covers | Size |
|------------|--------|------|
| 0–5 | whole world (1365 tiles) | about 100 MB; 14 MB with `--palette` |
| 0–6 | whole world or a continent | tens of MB |
| 0–8 | a country or region | about 100–300 MB |
| 0–10 and above | a small bbox only (a few degrees across) | hundreds of MB and up; each extra level is about 4x the previous |

## Taking them to the receiver

`scripts/make-offline-bundle.sh` adds `www/tiles/` to the bundle when
it exists, prints its size, and warns when it is over 500 MB. On the
receiver the build does not install tiles; after building, copy them next to
the installed UI:

```sh
cp -r www/tiles build/stage/share/skycue/www/     # or <prefix>/share/skycue/www/
```

skycued reads the directory on each `/api/tiles-available` request, so a
reload of the page picks the tiles up without a restart.

## Packages and source installs: skycue-make-maps

The Debian and RPM packages (and a `cmake --install` of `skycue/`) ship no
tiles. They install this script, `make-overlays.sh` and `tiles-merge.py` in
`<prefix>/share/skycue/scripts/`, and `skycue-make-maps` in `<prefix>/bin/`,
which runs them with the map directory and cache given explicitly (the
scripts' own defaults point into the source tree). It works the regions the
same additive way as the runs above: the whole world at zoom 0–5 as region
`global` (only when `<MAP_DIR>/tiles/meta.json` lacks it), then the region
asked for, merged in, then the overlays:

```sh
sudo skycue-make-maps --around 150         # global + 150 nm around RECEIVER_LAT/LON, zoom 6-9
sudo skycue-make-maps --preset conus       # adds -125,24,-66,50 at zoom 6-8
sudo skycue-make-maps --preset europe      # adds -11,35,31,61 at zoom 6-8
sudo skycue-make-maps --bbox -5,50,2,55 --zoom 6-10   # any box (region "bbox")
skycue-make-maps --list                    # the regions in meta.json
```

Each preset is about 1,300 tiles at zoom 6–8, roughly as many as the base,
so about 200 MB with it (see "Size and zoom" above). It reads `MAP_DIR` (and,
for `--around RADIUS_NM`, the receiver position) from
`/etc/skycue/skycue.conf` or `--config FILE`, or takes `--map-dir DIR`; uses
the GDAL on PATH, or `--container [IMAGE]`; and as root gives the map
directory to the `skycue` user. See "Map tiles" in
[docs/operating.md](../docs/operating.md) for every option.

`--around` makes a box RADIUS_NM out from the point in each direction and
clamps it to the map's extent (longitude -180 to 180, latitude -85 to 85).
It does not wrap across the antimeridian: a receiver near 180° longitude
gets only the side of the box on its own side of the line (for example
`--around 60 179.5 300` gives `169.5,55,180,65`, where a wrapped box would
also cover -180° to -170.5°). Bake the other side as a second region with
`--bbox -180,55,-170.5,65 --zoom 6-9` if you need it. A box near a pole is cut at
±85° in the same way.

It refuses a map directory that is `/` or that already holds other files
without a `tiles/` or `overlays/` directory (exit 2), before it writes
anything or changes the owner of anything.

## Borders and city names

Country and state borders and city names are not in the tiles: they are a
separate, optional overlay layer (about 1 MB) the page draws on top, in the
Light and Dark map styles alike. `scripts/make-overlays.sh` makes them from
Natural Earth vectors; see "Map style and overlays" in
[docs/operating.md](../docs/operating.md).

## Attribution

Made with Natural Earth. Free vector and raster map data @
naturalearthdata.com. The data is in the public domain; no attribution is
required, but the map credits it ("Made with Natural Earth") when tiles are
present.
