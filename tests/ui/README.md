# Headless browser tests of the web UI (dev machines only)

`ui.spec.js` drives the page in headless Chromium with Playwright against a
real skycued replaying `../fixtures/readsb-sample.jsonl` in real time. It
is a development check: node, npm and Playwright are never needed by a
package build or the containers. A build that does not want them configures
skycue with `-DSKYCUE_UI_TESTS=OFF`, and otherwise the ctest
`ui_playwright` prints `SKIP: playwright not installed` and is reported as
skipped (exit 77) when `node_modules/@playwright/test` is missing.
`SKYCUE_UI_NODE_MODULES=/path/to/node_modules` points it at another
`node_modules` (put first on `PATH` and `NODE_PATH`) instead.

## One-time setup (needs the network)

```sh
cd tests/ui
npm install                # @playwright/test, exact version from package.json / package-lock.json
npx playwright install chromium   # only if the matching browser is not already installed
```

`package.json` pins `@playwright/test` 1.61.1, whose Chromium is revision
1228 (`~/.cache/ms-playwright/chromium-1228`, or `PLAYWRIGHT_BROWSERS_PATH`).
When that browser is already there, `PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1 npm
install` downloads nothing else. `node_modules/` is git-ignored.

## Running

Through ctest, from the skycue source directory, with a build configured with Leaflet and
ECharts (`run.sh` says SKIP without either; see `www/README.md`):

```sh
cmake -S . -B build -DSKYCUE_LEAFLET_DIR=/path/to/leaflet/package/dist \
  -DSKYCUE_ECHARTS_DIR=/path/to/echarts/package
cmake --build build
ctest --test-dir build -R ui_playwright --output-on-failure
```

or directly: `bash run.sh <skycued> <fixtures dir> <build>/www-stage <skycue-pingsim> [playwright args]`
with absolute paths (the reconnect case restarts skycued from the spec's
directory); extra arguments go to `playwright test`, e.g. `--grep
range_change --project desktop-1920`. Every daemon it started is stopped
however it ends, including on SIGINT, SIGTERM or SIGHUP.

`run.sh` writes a temporary `skycue.toml` with free `web.port` and
`cues.tcp_port`, starts skycued with `--replay ... --replay-speed 1
--replay-loop --www-dir <www-stage>`, exports `SKYCUE_BASE_URL`,
`SKYCUE_CONF_PATH`, `SKYCUE_START_CMD` and `SKYCUE_PID_FILE` (the reconnect
case kills skycued and starts it again with that command), runs `npx
playwright test` (projects at 1280x720 and 1920x1080, one test at a time),
stops skycued and copies the `skycue-*.png` screenshots to
`<build>/ui-screenshots/` (override with
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
`<build>/fisb-screenshots/` (override with
`SKYCUE_FISB_SCREENSHOTS`).

For the Feeds tab cases it starts one more skycued, `--no-readsb
--no-uat` with a temporary `recording.dir` and an inputs file holding one
`pingstation_json` input, `ps-sim`, on a free UDP port, and
`skycue-pingsim` sending the readsb fixture to it as pingStation JSON
(`--loop --speed 5`, a device status every 2 s at 38.9 N 77.0 W), as
`SKYCUE_RECEIVERS_URL`. The cases there leave its inputs as they found them.

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
   confirmation, clears the overlay and writes `filters.max_range_nm` to `skycue.overrides.toml`.
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
the www directory (without its `tiles/`) with generated tiles while
`meta.json` says zoom 0..6 (`SKYCUE_TILES_URL`): `2/1/1` in four coloured
quadrants, and the receiver's zoom-6 tile `6/18/24`, transparent in its west
half, which lies outside its region's bbox.

19. `missing_tile_shows_lower_zoom_fallback`: every zoom-6 tile on the map
    is loaded; each missing one shows `tiles/2/1/1.png` enlarged 16 times
    and shifted to that tile's own place in it (left/top = -(x mod 16, y
    mod 16) x 256 px, a different place for each tile); `6/18/24` shows
    itself over the same parent (`tile-underlay`), since it is on its
    region's edge; no page error.

Feeds tab, against the receivers skycued (`SKYCUE_RECEIVERS_URL`, ps-sim
fed by skycue-pingsim), each case removing what it added:

20. `inputs_header_opens_receiver_tab`: `#stat-inputs` reads `1/1`; a click
    opens the Feeds tab, the tabs switch, and a second click closes it.
21. `receiver_tab_lists_inputs_with_live_counts`: the ps-sim card turns
    green, shows its endpoint, obs/s above 0 and the device line (GPS 3D
    fix, receiver normal, firmware 1.0.0); its `.device-marker` is on the
    map; Show pans to it keeping zoom 7, and from zoom 3 far away zooms to 6.
22. `add_input_form_validates_and_shows_server_error`: id `PS` and port 0
    are marked inline without a request; ps-sim's port is sent and the
    form shows skycued's 400 message word for word.
23. `add_edit_disable_remove_input`: add a CAT021 udp input through the
    form, rename it, disable it from its card while the form edits it (Save
    keeps it disabled, grey dot), and remove it (a dismissed confirm keeps
    it); `/api/inputs` agrees at each step.
24. `removed_id_added_again_starts_fresh`: ps-sim removed and added again
    on a silent port shows "no data yet", not the old input's data age.
25. `input_name_is_text_not_html`: a name and a server error made of an
    `<img onerror>` tag show as text; no `img` element, no script ran.

Feeds tab, against the 2015 FIS-B skycued:

26. `ground_stations_listed`: one station row (id, position, products) and
    one `.station-marker`.
27. `decoder_health_table`: the decoder counters as in `/api/fisb/status`,
    `uplink_duplicates` 0, the last uplink time and recorder drops.

Every test also fails if the page requested anything from another host
than the one it was loaded from (`data:` URLs, the radar images, aside).
