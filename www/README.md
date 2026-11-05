# skycue web UI

The page skycued serves on `WEB_PORT` (see `docs/web-api.md`): `index.html`,
`app_pure.js` (pure helpers, `window.skycuePure`, also loaded by the node
test), `alerts_pure.js` (the alert rules engine, `window.skycueAlertsPure`,
node-tested by `tests/js/alerts_pure.test.js`), `alerts_view.js` (the alert
strip, built from a host object `app.js` passes in, like `stats_view.js`),
`app.js` (the page) and `style.css`, with the Stats view's scripts
([Stats view](#stats-view)). Plain ES2017, no framework, no build step, and
no request to any host but skycued itself.

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

## ECharts

The statistics charts use Apache ECharts 5.6.0 (Apache-2.0), pinned in
`cmake/sources.cmake` as the npm registry tarball (`echarts-5.6.0.tgz`,
which ships the prebuilt `dist/echarts.min.js`; only extracted, never run
through npm). It is not committed: the page loads it from
`vendor/echarts/echarts.min.js`, and `LICENSE`, `NOTICE` and
`licenses/LICENSE-d3` are installed beside it.

- **Superbuild**: the `echarts` step extracts the tarball and the skycue
  step installs those files into `stage/share/skycue/www/vendor/echarts/`.
- **Standalone skycue build**: pass the extracted package root,
  ```sh
  mkdir -p /tmp/echarts && tar xzf sources/echarts-5.6.0.tgz -C /tmp/echarts \
    package/dist/echarts.min.js package/LICENSE package/NOTICE package/licenses/LICENSE-d3
  cmake -S skycue -B build-skycue -DSKYCUE_ECHARTS_DIR=/tmp/echarts/package
  ```
  (with `-DSKYCUE_LEAFLET_DIR` as above). Without `SKYCUE_ECHARTS_DIR` the
  page has no chart library and the tests that need it say SKIP.

## Stats view

The "Stats" header button shows `#stats-view`, the third main view beside
the map and the table (`showMain` in `app.js`). `stats_view.js` polls
`/api/stats/*` while it is shown (every 10 s with "live"), `stats_pure.js`
shapes the JSON (node-tested in `tests/js/stats_pure.test.js`; under node
it loads `./app_pure.js` itself, in the page it uses `window.skycuePure`),
and `chart_theme.js` registers the ECharts theme `skycue` from the `:root`
colour tokens. `skycueChartTheme()` also returns `decision` (the decision
colours) and `bands`, the eight altitude-band colours of the coverage
charts in `/api/stats/coverage`'s band order, warm low to cool high:
`0-2.5k` #ff7b72, `2.5-5k` #f0883e, `5-10k` #e3b341, `10-20k` #7ee787,
`20-30k` #56d4dd, `30-40k` #58a6ff, `40k+` #d2a8ff, `unknown` #6e7681.
The chosen range is kept in localStorage `skycue.stats.range`. Charts are
created once and updated with `setOption`; they are disposed after the
view has been hidden for a minute.

Each section (Traffic, Weather, Coverage, Fleet, Signal, Aircraft) is one
`registerSection` call in `stats_view.js`'s `create`, run after every
refresh with the newest `summary` and `series`; the hourly series,
`coverage` and `rssi` are fetched on open, after a reset and every 6th
refresh. The Fleet clicks and the aircraft detail's buttons reach the map
through the host object `app.js` builds (`setViewFilter`, `select`,
`showMain`). What the view shows, for operators, is in
`docs/operating.md` (Stats view, Statistics file). The CSV shapes
(`csvText`, `ledgerCsvRows`, `seriesCsvRows`, `coverageCsvRows`,
`csvFileName`) are in `stats_pure.js`; the file is handed to the host's
`download`.

The alert strip (`#alerts`, over the map, the table and this view) is the
same pattern: `alerts_pure.js` holds the rules (`RULES`), the
edge-triggered engine (`createEngine`: dedupe, 5-minute quiet period,
10 s input hold-off, the snapshot baseline), the normalisers of
localStorage `skycue.alerts` and `skycue.alertLog` and the settings file
of the filter sets' Export…/Import… (`settingsFile`, `parseSettings`,
`mergeSettings`); under node it loads `./app_pure.js` itself, in the page
it uses `window.skycuePure`. `alerts_view.js` draws the strip, the drawer
and `#alerts-config` and plays the tones, from the host object `app.js`
builds (`dataNow`, `storeGet`/`storeSet`, `request`, `selectTrack`,
`openReceiver`, `openStatsAircraft` and the rest, listed at its top). What
the strip does, for operators, is in `docs/operating.md` (Alerts).

`index.html` loads the scripts in this order: Leaflet, ECharts,
`app_pure.js`, `alerts_pure.js`, `stats_pure.js`, `chart_theme.js`,
`stats_view.js`, `alerts_view.js`, `app.js`: each pure file after
`app_pure.js`, each view before `app.js`, which creates them.

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
closes the panel on that tab (`#panel-buttons` has one for each tab but
Track, and `#stat-inputs` is a second one for Receiver; `syncTabToggles`
presses the ones whose tab is showing), and `TAB_SHOWN[<name>]` in `app.js` draws the
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

## Display filters

The left panel is `#output-filters` (range, elevation, ring, the allow and
block lists and Save as baseline: what is sent to cue clients), then `#view`
("Display filters"), then the legend. `#view` holds "Hide filtered" and the
per-browser view settings (`normalizeView`, localStorage `skycue.view`);
`viewMatches` decides a track, `trackClass` and `trackCountry` estimate its class and
country, and `viewShows` in `app.js` exempts the selected track.
The Trails group (`#view-trails`) is part of the view: `trailShow`
(`P.TRAIL_SHOWS`; `trailShown` in `app.js`) says which entries have a
trail. An entry without one has no `path.trail` at all (`e.trail` null;
`drawTrail` makes or removes it, `renderEntry` calls it when that changes,
as on a selection with "selected"), while `e.pts` keeps filling and
ageing out. `P.viewTrailsDefault` is what Clear's enabled state needs
besides the badge count; a change to a Trails field redraws every trail
(`applyTrailOptions`).
Colour by (`#view-trail-color`, `trailColor`, `P.TRAIL_COLOR_BY`): each
trail point keeps its `rssi` (`P.signalLevel`) and its altitude
(`P.trackAlt`); with "signal" or "altitude" `drawTrail` lays
`P.trailRuns(points, P.trailRunColor(mode))` (runs of one step of
`P.signalColor`, `SIGNAL_COLORS` from `SIGNAL_MIN_DB` to `SIGNAL_MAX_DB`, or
of `P.altitudeColor`, `ALT_COLORS` split at `ALT_BOUNDS_FT`) as
`path.trail-run` polylines over the decision-coloured `path.trail`, which is
then transparent. A run without a value is solid in `P.TRAIL_NONE_COLOR`
(neutral grey) at a lower opacity; no trail polyline is dashed.
`P.viewSetsFillTrails` (in `initTrails`, after the `skycue.trailSignal`
take-over) gives stored sets without the Trails fields the current ones,
once, and saves the store. `#trail-signal-legend` is
`P.signalLegend`, `#trail-alt-legend` `P.altitudeLegend`. The runs are kept
up to date in place (`updateTrail`: `P.trailRunsAppend` on a new position,
`P.trailRunsTrim` on a prune), and rebuilt only on a Trails change, a new
marker, an un-dimming or a trail of two points or fewer. The old
checkbox's localStorage `skycue.trailSignal` is read once (`initTrails`:
"1" becomes "signal" unless the stored view already has a `trailColor`)
and removed.
Project ahead (`#view-trail-ahead`, `trailAhead`, minutes in
`P.TRAIL_AHEAD_MIN`, 0 off) is `e.ahead`, a dashed `path.trail-ahead` of
`P.projectAhead(track, minutes)`: from the position along `track_deg` for
`P.aheadNm(gs_kt, minutes)`, great-circle points (`P.projectPoint`, which
is `destinationPoint`) at most `AHEAD_STEP_NM` apart. `updateAhead` makes,
moves or removes it on every `renderEntry` and `drawTrail`; only an entry
with a trail has one, and `removeTrail` takes it too.

## Ruler

The "Measure" button (`#ruler-toggle`, a `leaflet-bar` control top left,
under the zoom buttons, so the alert strip's lift of `.leaflet-bottom` never
reaches it) arms `ruler` in `app.js`: every click handler on the map (the map, the
aircraft, the FIS-B shapes and symbols, the device and station markers)
asks `rulerTakes` first, which gives the point to `rulerClick` instead,
the document's Escape clears it instead of closing the panel, `showMain`
clears it when the map is covered, and the label
is `P.rulerLabel(P.rulerMeasure(...))` (`rangeNm` and `bearingDeg`).

## Cues tab

The Cues tab (`showCues`) shows skycued's output side. `#cues-counters` has
one row per `CUE_COUNTERS` entry, from every `stats` item. Every `update`
and `drop` WebSocket item becomes a `decisionRow` pushed into a ring of the
last `DECISIONS_RING` (200) with `pushRing`, whether the tab is shown or
not; `#cues-recent` lists it newest first (with `#cues-drops-only`, a
localStorage view option `skycue.cues.dropsOnly`, a second ring of drops
alone) and a row click selects the track
(`data-key` is `trackKey`). `#cues-filters` is `GET /api/filters/history`
through `filterHistoryRows`, which diffs consecutive entries (newest first;
the startup entry has every field, from `null`), and `#cues-clients` is
`GET /api/cues/clients` (`tr.tcp`, `tr.udp`). While the tab is shown both
are fetched again every 5 s, and the history also on every `filters` item.
Origins, endpoints and callsigns are set with `textContent`.

## Config tab

The Config tab (`showConfig`) is read only and fetches `GET /api/config`
each time it is shown and on `#config-refresh`. `#config-daemon` lists the
daemon facts (the run mode through `daemonModeText`, the learned and
operator locations through `daemonLocationsText`); `groupConfig` sorts the keys into one `table[data-group]`
per `CONFIG_GROUPS` entry (a key no group names goes to a trailing
`Other`, an empty group is left out), each key a `tr[data-key]` with
`td.cfg-value`, `td.cfg-source` and `td.cfg-restart` (a `restart` badge
when the key needs one). `#config-copy` writes `configText` (skycue.conf
text, quoted as `conf_rewrite.cpp`'s `quoteValue` does, each key preceded
by a `# KEY: source[, restart]` comment line so the text parses as it is;
`confValue` returns null for a value the grammar cannot hold, which is left
as a comment) with `navigator.clipboard.writeText`;
`#config-copy-status` says `Copied`, or `Copy failed` with the text shown
selected in the read-only `#config-copy-text`. Keys, values and paths are
set with `textContent`.

## Geometry

`elevationDeg` in `app_pure.js` duplicates `skycued/src/geo.cpp`
(haversine on a 6371008.8 m sphere, 4/3 effective radius in the curvature
term) so the elevation ring matches what the filters use. Change both
together; `tests/js/app_pure.test.js` checks it against values from
`tests/test_geo.cpp`.
