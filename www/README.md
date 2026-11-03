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

`vendor/`, `tiles/` and `overlays/` (scripts/make-overlays.sh) in this directory are git-ignored, for local use
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
product dropdown, legend marked approximate; icing and turbulence also get
an altitude band dropdown, `≈ 4,000 ft` per 3-bit code, bands without data
disabled, defaulting to the lowest band with data, the legend title naming
the band; `rasterProducts`, `defaultBand`) and one layer per shape family (NOTAM / TFR,
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

Weather symbols on the map: a METAR is a filled circle coloured by flight
category (VFR green, MVFR blue, IFR red, LIFR magenta, grey when visibility
or ceiling cannot be parsed) with a wind barb (a ring when calm, no barb
when variable); a TAF an outlined square right of it, coloured by its first
group; a PIREP a triangle (filled UA, outlined with "!" for UUA); winds aloft
a barb with the temperature, drawn just below the station, from the record
of the picked forecast period at the picked level. The period picker (shown
with the Winds toggle) offers the periods present (6, 12, 24 h), defaulting
to the one whose valid time is nearest the data clock; the level picker the
levels present in that period, defaulting to the lowest; a pick stays until
it is no longer offered (`windsPeriods`, `windsLevelsFor`). Other winds rows,
and stations whose table lacks the level, say "no barb" and have no symbol. Symbols fade
with the list's age classes and go when their record expires; hover shows
the station, a click the parsed report and raw text. The Reports toggles
(METAR, TAF, PIREP, Winds, each with its count) and the station filter apply
to the list and the symbols alike. Positions come from `/api/fisb/locations`
and `fisb_location` (ids learned from NOTAM points, or `LOCATIONS_FILE`): a
report whose station has no known position is listed with "no position" and
never plotted, and the Reports header shows "n of m positioned" (n: the
reports with a symbol); those rows get a "show" link that pans to the symbol.
Only symbols inside the view (grown by a quarter on each side) are drawn, at
most 300: METAR first, then TAF, PIREP and winds, each nearest the map centre
first. They are redrawn at most once a second, only on a change (a record, a
location, a toggle, the filter, a pan or zoom, the age tick).

## Side panel tabs and the Receiver tab

The side panel's tabs are its `[role=tab]` buttons: `tab-<name>` shows
`pane-<name>`, a header button with `data-opens-tab="<name>"` opens and
closes the panel on that tab, and `TAB_SHOWN[<name>]` in `app.js` draws the
tab when it is shown. A new tab is its markup in `index.html` and one entry
in `TAB_SHOWN`.

The Receiver tab (docs/web-api.md, Inputs) takes the input list from the
snapshot, the `inputs` WebSocket items and `GET /api/inputs` every 5 s (the
device status, status-poll failures and data ages are not changes), and the
live counters from every `stats` item (`inputRates`). One card per input
with its status dot (`inputDot`), endpoint (`inputEndpoint`) and, for a
pingStation, the device line (`statusStale`); the header's `inputs n/m` is
`inputsSummary`. The add/edit form shows `inputFields(type, transport)` and
checks `validateInputDef`, which mirrors skycued's `parseInputDef` rules and
messages, before it sends; skycued's 400 message is shown as it comes.
Device positions are `.device-marker` markers, FIS-B ground stations
`.station-marker`. Everything from an input's definition, the device or a
server error is set with `textContent`, never as markup.

## Cues tab

The Cues tab (`showCues`) shows skycued's output side. `#cues-counters` has
one row per `CUE_COUNTERS` entry, from every `stats` item. Every `update`
and `drop` WebSocket item becomes a `decisionRow` pushed into a ring of the
last `DECISIONS_RING` (200) with `pushRing`, whether the tab is shown or
not; `#cues-recent` lists it newest first and a row click selects the track
(`data-key` is `trackKey`). `#cues-filters` is `GET /api/filters/history`
through `filterHistoryRows`, which diffs consecutive entries (newest first;
the startup entry has every field, from `null`), and `#cues-clients` is
`GET /api/cues/clients` (`tr.tcp`, `tr.udp`). While the tab is shown both
are fetched again every 5 s, and the history also on every `filters` item.
Origins, endpoints and callsigns are set with `textContent`.

## Geometry

`elevationDeg` in `app_pure.js` duplicates `skycued/src/geo.cpp`
(haversine on a 6371008.8 m sphere, 4/3 effective radius in the curvature
term) so the elevation ring matches what the filters use. Change both
together; `tests/js/app_pure.test.js` checks it against values from
`tests/test_geo.cpp`.
