# Headless browser tests of the web UI (dev machines only)

`ui.spec.js` drives the page in headless Chromium with Playwright against a
real skycued replaying `../fixtures/readsb-sample.jsonl` in real time. It
is a development check: node, npm and Playwright are never needed by the
superbuild, the offline bundle or the containers. The superbuild configures
skycue with `-DSKYCUE_UI_TESTS=OFF`, and in a standalone build the ctest
`ui_playwright` prints `SKIP: playwright not installed` and passes when
`node_modules/@playwright/test` is missing.

## One-time setup (needs the network)

```sh
cd skycue/tests/ui
npm install                # @playwright/test, exact version from package.json / package-lock.json
npx playwright install chromium   # only if the matching browser is not already installed
```

`package.json` pins `@playwright/test` 1.61.1, whose Chromium is revision
1228 (`~/.cache/ms-playwright/chromium-1228`, or `PLAYWRIGHT_BROWSERS_PATH`).
When that browser is already there, `PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1 npm
install` downloads nothing else. `node_modules/` is git-ignored.

## Running

Through ctest, from a standalone skycue build configured with Leaflet:

```sh
cmake -S skycue -B build-skycue -DSKYCUE_LEAFLET_DIR=/path/to/leaflet/package/dist
cmake --build build-skycue
ctest --test-dir build-skycue -R ui_playwright --output-on-failure
```

or directly: `bash run.sh <skycued> <fixtures dir> <build>/www-stage`.

`run.sh` writes a temporary `skycue.conf` with free `WEB_PORT` and
`CUE_TCP_PORT`, starts skycued with `--replay ... --replay-speed 1
--replay-loop --www-dir <www-stage>`, exports `SKYCUE_BASE_URL`,
`SKYCUE_CONF_PATH`, `SKYCUE_START_CMD` and `SKYCUE_PID_FILE` (the reconnect
case kills skycued and starts it again with that command), runs `npx
playwright test` (projects at 1280x720 and 1920x1080, one test at a time),
stops skycued and copies the `skycue-*.png` screenshots to
`.superpowers/sdd/2026-11-01-skycue-ui/screenshots/` (override with
`SKYCUE_UI_SCREENSHOTS`). Results go to `<build>/ui-test-results/`.

For the FIS-B cases it starts two more skycueds, each replaying a received
recording with uplinks unpaced (`--replay-speed 0`, no loop) and waits
until every uplink is decoded and the NEXRAD published:
`../fixtures/fisb/received-with-uplinks.jsonl` (2015: METAR/TAF/PIREP/winds,
point NOTAMs, a SIGMET; receiver at its ground station, 37.32 N 121.75 W)
as `SKYCUE_FISB_URL`, and `../fixtures/fisb/received-tg30.jsonl` (2020:
NEXRAD regional and CONUS with echoes, rasters, AIRMETs; receiver at its
ground station, 40.04 N 86.26 W, inside both mosaics) as
`SKYCUE_RADAR_URL`. Their `fisb-*.png` screenshots go to
`.superpowers/sdd/2026-11-02-skycue-fisb/screenshots/` (override with
`SKYCUE_FISB_SCREENSHOTS`).

## Cases

1. `page_loads_without_console_errors`: no console error or page error;
   the Leaflet map, the graticule (no tiles) and the scale bar are there.
2. `markers_appear_for_all_tracks`: at least 10 aircraft markers, all of
   them tracks skycued has; the stats strip shows at least 10 tracks.
3. `click_marker_opens_panel_with_fields`: the panel shows every field with
   units, updates live, and Esc closes it.
4. `add_to_block_list_drops_and_recolours`: blocking a forwarded aircraft
   from the panel lists it, turns its marker red, stops forwarding it (it
   keeps moving on the map), and removing it restores all of that.
5. `range_change_moves_ring_and_drops`: typing in Max range sends nothing
   (an invalid value is marked); Enter on 20 sends one `PUT /api/filters`,
   shrinks the range ring, and the marker colours agree with `/api/state`.
6. `save_baseline_confirms_and_clears_overlay`: Save as baseline asks for
   confirmation, clears the overlay and writes `MAX_RANGE_NM` to the conf.
7. `reconnect_after_restart_converges`: kill and restart skycued; the
   banner appears and clears, the markers come back from the new snapshot
   and a marker skycued does not know is gone.
8. `stalled_server_detected_and_reconnects`: skycued paused (SIGSTOP) for
   7 s, so its connection stays open but silent: the banner shows
   "disconnected" within 7 s (no stats for 5 s), and after SIGCONT the page
   reconnects.
9. `screenshots`: map with a selection, and the table sorted by range.

FIS-B, against the tg30 skycued:

10. `radar_canvas_drawn_when_radar_present`: radar layers on and every
    other layer off by default; both radar image overlays exist, their
    canvases have painted pixels (`skycueApp.radarStats()`), cover the
    mosaic's box, and the radar age shows; the layer toggle removes and
    restores the overlay; no console error.
11. `radar_opacity_slider`: the slider sets the overlays' opacity.
12. `other_rasters_layer_and_legend`: the raster layer draws the product
    chosen in the dropdown, with its legend marked approximate.
13. `fisb_screenshots_radar`: radar, SIGMETs and the Weather tab.

FIS-B, against the 2015 skycued:

14. `tfr_polygon_rendered_and_clickable`: the TFR layer draws every
    NOTAM shape; clicking one shows its text, key and times in the panel.
    The fixtures have no TFR polygon, so live items injected through the
    page's own handler add a polygon, a circle (radius from `radius_nm`)
    and a cancelled (dashed) shape, replace a record's shapes by key, and
    `fisb_expired` removes them and closes their details.
15. `weather_panel_lists_metars_and_filters`: the Weather tab lists every
    METAR (and TAF, PIREP, winds with `valid`), each row with its issue
    time and age class; a row expands to its text; a 3-letter station
    prefix narrows the list; `fisb_text` adds a row and `fisb_expired`
    removes rows.
16. `layer_toggles_hide_show`: the SIGMET layer's paths appear and go.
17. `fisb_status_strip`: `uplinks/min`, `products/min`, `radar age` and
    `stations` in the strip, stations as in `/api/fisb/status`.
18. `fisb_screenshots_weather`: a clicked NOTAM, SIGMET and a filtered list.

Map tiles, against a fourth skycued (the readsb fixture) serving a copy of
the www directory whose `tiles/` holds only a generated solid-colour zoom-0
tile while `meta.json` says zoom 0..4 (`SKYCUE_TILES_URL`):

19. `missing_tile_shows_lower_zoom_fallback`: every tile on the map is
    loaded and shows `tiles/0/0/0.png` enlarged 16 times (the zoom-4 to
    zoom-1 URLs 404 and are remembered), none failed, no page error.

Every test also fails if the page requested anything from another host
than the one it was loaded from (`data:` URLs, the radar images, aside).
