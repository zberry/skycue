# skycue web UI

The page skycued serves on `WEB_PORT` (see `docs/web-api.md`): `index.html`,
`app_pure.js` (pure helpers, `window.skycuePure`, also loaded by the node
test), `app.js` (the page) and `style.css`. Plain ES2017, no framework, no
build step, and no request to any host but skycued itself.

## Leaflet

The map library is Leaflet 1.9.4, pinned in `cmake/sources.cmake` as the
npm registry tarball (`leaflet-1.9.4.tgz`: it ships the prebuilt `dist/`,
which the GitHub source archive does not; it is only extracted, never run
through npm). It is not committed: the page loads it from
`vendor/leaflet/leaflet.js` and `vendor/leaflet/leaflet.css`.

- **Superbuild** (`cmake -S . -B build && cmake --build build`): the
  `leaflet` step extracts the tarball and the skycue step installs
  `dist/leaflet.js`, `dist/leaflet.css` and `dist/images/` into
  `stage/share/skycue/www/vendor/leaflet/`, next to these files.
- **Standalone skycue build**: pass the extracted `dist/` directory,
  ```sh
  mkdir -p /tmp/leaflet && tar xzf sources/leaflet-1.9.4.tgz -C /tmp/leaflet package/dist
  cmake -S skycue -B build-skycue -DSKYCUE_LEAFLET_DIR=/tmp/leaflet/package/dist
  ```
  The `www_stage` target (part of `all`) assembles these files plus Leaflet
  in `<build>/www-stage/`, which the web tests serve with `--www-dir`, and
  `cmake --install` puts them in `<prefix>/share/skycue/www/`. Without
  `SKYCUE_LEAFLET_DIR` the page has no map library and the tests that need
  it say SKIP.

`vendor/` and `tiles/` in this directory are git-ignored, for local use
only; the build never copies them.

## Map tiles

When `www/tiles/<z>/<x>/<y>.png` exist (see `/api/tiles-available`) the page
shows them; otherwise it draws a plain background with a 1°/5° graticule
and a scale bar. The tiles may come from several regions with different
zoom ranges (`scripts/make-tiles.md`); where the zoom shown has no tile, the
page's tile layer (`FallbackTileLayer` in `app.js`) shows the nearest
lower-zoom ancestor that exists (at most 4 levels down), enlarged
(`parentTile` in `app_pure.js`), and draws that ancestor underneath a tile
on a region's edge (`tileInsideRegions`, from `tiles/meta.json`).

## FIS-B weather

From the `fisb` status in the snapshot the page GETs `/api/fisb/text`,
`/api/fisb/shapes` and the NEXRAD mosaics, then applies the `fisb_*`
WebSocket items (docs/web-api.md, FIS-B). Map layers (the layer control,
top right): "Radar (regional)" and "Radar (CONUS)" (on by default), "Other
rasters" (lightning, icing, turbulence, cloud tops: one at a time from a
dropdown, legend marked approximate) and one layer per shape family (NOTAM / TFR,
SIGMET, AIRMET/G-AIRMET, CWA, SUA; off by default). A mosaic's blocks are
expanded from their RLE and painted on one canvas over their box (rows
spaced in Web Mercator, at most 4096 x 4096), shown as an image overlay;
the opacity slider and radar age sit bottom right, and a radar older than
15 min (regional) or 30 min (CONUS) is drawn at half opacity
(`radarOpacity`). Cancelled shapes are dashed, shapes not yet in force
(`pending`) dotted and dimmed (`shapeStyle`). Clicking a shape shows its text, times and
altitudes in the side panel's Weather tab, which also lists METAR, TAF,
PIREP, winds aloft and the TWGO reports without graphics ("NOTAM / SIGMET
text") with a station-prefix filter and age colouring (fresh < 30 min,
aging < 90 min, old). A TWGO text item without graphics clears the shapes
drawn for its key (an amended text drops the old graphics).

## Geometry

`elevationDeg` in `app_pure.js` duplicates `skycued/src/geo.cpp`
(haversine on a 6371008.8 m sphere, 4/3 effective radius in the curvature
term) so the elevation ring matches what the filters use. Change both
together; `tests/js/app_pure.test.js` checks it against values from
`tests/test_geo.cpp`.
