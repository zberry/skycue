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
8. `screenshots`: map with a selection, and the table sorted by range.

Every test also fails if the page requested anything from another host.
