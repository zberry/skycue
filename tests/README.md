# Tests

## Families

- `test_*.cpp`: Boost.Test unit tests. The ctest name is the file name without
  `test_`, and related tests share a prefix: `decode_`, `fisb_`, `hub`, `web_`;
  the `net_` tests live in `net/tests`.
- `test_*.sh`: shell tests, many of which start `skycued`. Most share
  `lib/test_common.sh`.
- `scripts/`: tests of the scripts and packaging, each registered as
  `script_<name>`.
- `js/`: node tests of the page's pure helpers, run by the ctest `web_ui`.
- `ui/`: the Playwright browser suite, ctest `ui_playwright`; setup is in
  `ui/README.md`.
- `fuzz/`, `soak/`, `sanitizer/`: opt-in tiers, see
  [docs/hardening.md](../docs/hardening.md).
- `fixtures/`: test inputs and goldens, see `fixtures/README.md`.

## Running one test

```sh
ctest --test-dir build -N                              # list every test
ctest --test-dir build -R '^name$' --output-on-failure # one test
ctest --test-dir build -R '^decode_'                   # a family
```

## Environment and cache variables

| variable | effect |
|---|---|
| `SKYCUE_TEST_SLOW_FACTOR` | CMake cache variable: multiplies test timeouts and waits (sets `SKYCUE_TEST_SLOW` in each test). Empty means 4 in a sanitizer or fuzz build, else 1 |
| `SKYCUE_TEST_SLOW` | the factor as the tests read it; set by the build |
| `SKYCUE_TEST_READSB` | CMake cache variable: a readsb executable for the full-stack part of `rawtools` |
| `SKYCUE_READSB` | environment: a readsb executable for the fresh run in `decode_differential` |
| `SKYCUE_DUMP978` | environment: a dump978-fa executable for the fresh run in `decode_differential_uat` |
| `SKYCUE_FUZZ` | CMake option: build the fuzz harnesses (clang only); enables `fuzz_smoke` |
| `SKYCUE_STATIC_TESTS` | CMake option: register `static_analysis` (needs clang-tidy) |
| `SKYCUE_SOAK_TESTS` | CMake option: register `soak_short` (needs node and curl) |
| `SKYCUE_UI_TESTS` | CMake option, default ON: register `ui_playwright` |
| `SKYCUE_WERROR` | CMake option: warnings are errors |

Never put a path to an external decoder in a file; pass it through these variables.

## Why tests skip

A skip is ctest's "Skipped" (exit status 77). It is not a pass.

| test | skips when |
|---|---|
| `parent_project` | the cmake it is given is not found |
| `package_conf`, `slow_factor`, `icao_tables_current`, `icao_tables_generator` | python3 is not found |
| `hub`, `fisb_cli` | the fetched captures are missing (`scripts/fixtures/fetch-test-captures.sh`); `hub` also needs python3 and curl |
| `geom_eval`, `decode_acas_opensky` | the fetched captures (`geom_eval`) or the OpenSky data (both) are missing |
| `decode_differential` | the captures, python3 or curl are missing; its fresh-run half also needs `SKYCUE_READSB` and is skipped without it |
| `decode_differential_uat` | python3 or curl is missing, or every sample is absent; its fresh-run half also needs `SKYCUE_DUMP978` |
| `decode_uat_golden`, `decode_uat_synthetic` | a committed golden file is missing (they do not need fetched captures) |
| `ui_playwright` | node or `npm ci` in `tests/ui` (Playwright 1.61.1, Chromium) is missing, or the web libraries are not staged |
| `fuzz_smoke`, `static_analysis`, `soak_short` | `SKYCUE_FUZZ`, `SKYCUE_STATIC_TESTS` or `SKYCUE_SOAK_TESTS` is off (clang for the first, clang-tidy for the second); `soak_short` also skips without the fetched FIS-B recording, node 22 or newer, or curl |
| `script_*` | the tool that script needs (python3, curl, a git checkout) is missing |

Two tests do not skip as a whole: `rawtools` and `iqgen_cli` print `SKIP` for the sections
that read the fetched captures, run the rest and report a pass. `web_ui` runs
its node checks only when node is installed and passes without them.

Any CI setup should include one job with the fetched fixtures and the external decoders set,
or the differential tests never run.
