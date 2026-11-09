# AGENTS.md

skycue is an operator console and cue forwarder for ADS-B/UAT receivers: it reads readsb and
dump978-fa output (and its own decoders), shows a live map, and forwards chosen aircraft as cues.
C++17, Boost, CMake; web page in plain JavaScript. Everything is built from source, offline.

## Layout
- `libskycue/`: client library (`skycue::skycue`), wire protocol, ECEF helpers.
- `skycued/`: the daemon: inputs, decoders, track table, engine/filters, hub, web server, recorder.
- `net/`: `skycue::net` socket library (dialer, listener, framer, fanout) with its own tests.
- `www/`: the web page. `app_pure.js` and friends are pure helpers tested under node.
- `tools/`: `skycue-*` command-line tools; `tools/iqgen/` makes IQ test files.
- `examples/`: client demos (minimal, ECEF callbacks, ECEF `takeChanges()`, parent project).
- `tests/`: Boost.Test unit tests, shell tests, `js/`, `ui/` (browser), `fuzz/`, `fixtures/`.
- `scripts/`: `dev/` (sanitizers, fuzz, static, fetch web libs), `fixtures/`, `maps/`, `lib/`.
- `packaging/`, `debian/`, `cmake/`: packages, systemd unit, CMake modules. `docs/`: documentation.

## Build and test
- Once per git clone: `scripts/dev/fetch-web-libs.sh` (fetches the Leaflet and ECharts tarballs into `sources/`).
- `cmake -S . -B build && cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure`
- One test: `ctest --test-dir build -R '^name$'`. Families: `-R '^decode_'`, `-R '^net_'`.
- JS tests run through the ctest `web_ui` (needs `node`; skipped without).
- Browser suite: ctest `ui_playwright`, on by default (`-DSKYCUE_UI_TESTS=OFF` removes it). It skips
  (exit 77) without `node_modules/@playwright/test`; see `tests/ui/README.md` for setup.
- Test families, variables and the reasons tests skip: `tests/README.md`. Contributor rules: `CONTRIBUTING.md`.
- Skips are normal: ctest reports exit 77 as Skipped. A skip is not a pass; say which tests skipped.
- Fetched fixtures: `scripts/fixtures/fetch-test-captures.sh`. Tests using `tests/fixtures/raw`,
  `fisb/tg30.978` and the UAT/Stratux captures skip without them (`decode_differential*`, `rawtools`,
  `hub`, FIS-B golden). IQ samples: `scripts/fixtures/fetch-iq-samples.sh`.
- External decoders (optional, for fresh reference runs): `SKYCUE_READSB` (readsb) and
  `SKYCUE_DUMP978` (dump978-fa) name executables for `decode_differential` and `decode_differential_uat`;
  `SKYCUE_TEST_READSB` (CMake cache variable) does the same for `rawtools`. Unset: the committed
  goldens stand and the live run is skipped. Never put a path to them in a file.
- Sanitizers, fuzzing, static analysis, soak: `docs/hardening.md`. Test fixtures: `tests/fixtures/README.md`.

## Dependencies (fixed set)
- Allowed: Boost, the vendored Leaflet and ECharts tarballs, the C++17 standard library, system libraries (zlib).
- Target machines have no network access and everything is built from source.
- Do not add a third-party library, npm package, Python package or header-only import without the
  maintainer's explicit decision. Use the in-tree helpers first: `json_lite`, `toml_lite`, `skycue::net`,
  and the test-support headers in `tests/` (`*_test_support.hpp`).
- Python and node are for scripts and tests only, standard library only.

## Clean-room rule
- Never read readsb, dump1090, dump978, pyModeS or stratux source. Running them and reading their
  output as a reference is allowed. Implement from public specifications and from this repo's docs
  (`docs/fisb-reference.md`, `docs/asterix-cat021-reference.md`).
- Do not paste or paraphrase code from them, in code, tests or comments.

## Conventions
- Test first; keep commits small, one change per commit, with a message that says why.
- Goldens: `tests/fixtures/gen_raw_golden.sh` (readsb, Mode S), `gen_uat_golden.sh` (dump978-fa),
  `fisb/gen_*.py` and `docs/fisb-golden.md` (FIS-B). Regenerate only when behaviour change is intended,
  and say so in the commit. Differential tests: `tests/test_decode_differential*.sh`.
- Keep in step: `packaging/skycue.toml.example` is the single example config, which the tests read directly;
  `docs/protocol.md` with the encoder in `libskycue/`; `docs/recording-format.md` with the recorder key
  tables; `docs/web-api.md` with the routes; `tests/fixtures/config-copy.toml` with `configText` in
  `www/app_pure.js`; `skycued/src/icao_tables.inc` via `scripts/fixtures/gen-icao-tables.py`.
- Never commit fetched third-party data (captures, OpenSky data, tarballs). They stay git-ignored.
- Never hard-code paths to external tools; take them from environment variables or arguments.
- The web page is plain JavaScript (ES2017 as `www/README.md` states) with no build step, no framework,
  and no request to any host but skycued. Match the style of `www/app.js`: `var`, function
  expressions, pure helpers in `*_pure.js` with node tests, views as `create(host)` modules.
- Warnings are errors in CI-style builds (`SKYCUE_WERROR=ON`); keep the build warning-free.

## Which doc to update
- Config key or behaviour: `docs/operating.md` and `packaging/skycue.toml.example`.
- HTTP/WebSocket API or web UI: `docs/web-api.md`, `docs/operating.md`, `www/README.md`.
- Wire protocol or library API: `docs/protocol.md`, `docs/libskycue.md`.
- Recordings: `docs/recording-format.md`. FIS-B: `docs/fisb.md`, `docs/fisb-reference.md`.
- Install, packaging: `docs/installing.md`, `docs/releasing.md`. Visible changes: `README.md`.
- New third-party material: `NOTICE`, `debian/copyright`, `tests/fixtures/README.md`.

## Don't
- Do not add dependencies, vendored code, or network access at build or run time.
- Do not read the decoders' source (see above); do not commit fetched data or generated build trees.
- Do not weaken or delete a test to make it pass; do not regenerate a golden to hide a regression.
- Do not add a login-less feature that mutates the system without documenting it (the web UI has no authentication).
- Do not commit machine names, absolute paths, usernames or details of any deployment.
- Do not edit `docs/` tables without updating the code they mirror, or the reverse.
