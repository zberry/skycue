# Contributing

Issues and pull requests are welcome on GitHub. `AGENTS.md` holds the same
rules in short form.

## Build and test

A git clone has no `sources/`. Fetch the web libraries (Leaflet and ECharts) once:

```sh
scripts/dev/fetch-web-libs.sh
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build
```

Run `ctest` before sending a change. Some tests skip themselves when an optional
tool or a fetched capture is missing. A skip is not a pass: say which tests
skipped. [tests/README.md](tests/README.md) lists the test families, the
variables that control them and why each test skips. The sanitizer, fuzzing,
static-analysis and soak runs are in [docs/hardening.md](docs/hardening.md);
build details are in [docs/installing.md](docs/installing.md#build).
CI runs `ctest -LE 'needs-fixtures|needs-node|slow'`, the tests that need neither fetched data, node nor minutes.

## Test first

Write a failing test, then the change. For a refactor, first add a test that pins
today's behaviour. Keep one change per commit, and let the message say why.
Keep pure moves of code in commits separate from edits.

## The fixed dependency set

Allowed: Boost, the vendored Leaflet and ECharts tarballs, the C++17 standard
library, system zlib and POSIX. Python 3 (standard library only) and node are for
scripts and tests only.

Do not add a third-party library, npm or pip package, `FetchContent` or vendored
code without the maintainer's explicit decision. Use the in-tree helpers first:
`json_lite`, `toml_lite`, `skycue::net` and the test-support headers in `tests/`
(`*_test_support.hpp`).

## Clean room

Never read the source of readsb, dump1090, dump978, pyModeS or stratux. Running
them and reading their output as a reference is allowed. Do not paste or
paraphrase their code, in code, tests or comments. Implement from public
specifications and from this repository's docs.

## The web page

The page is plain ES2017 with no build step and no framework, as
[www/README.md](www/README.md) states. Match `www/app.js`: `var` and function
expressions, pure helpers in `*_pure.js` with node tests under `tests/js/`.

## Which doc to update

| you change | update |
|---|---|
| a config key or behaviour | `docs/operating.md` and `packaging/skycue.toml.example` |
| the HTTP/WebSocket API or the web UI | `docs/web-api.md`, `docs/operating.md`, `www/README.md` |
| the wire protocol or library API | `docs/protocol.md`, `docs/libskycue.md` |
| recordings | `docs/recording-format.md` |
| FIS-B | `docs/fisb.md`, `docs/fisb-reference.md` |
| install or packaging | `docs/installing.md`, `docs/releasing.md` |
| something users can see | `README.md` |
| new third-party material | `NOTICE`, `debian/copyright`, `tests/fixtures/README.md` |

## Licence

skycue is Apache-2.0. Contributions are accepted under the same licence
(section 5 of the licence: what you submit is licensed as the project is).
There is no CLA.

## Never commit

Fetched third-party data (captures, OpenSky data, tarballs), machine names,
absolute paths, usernames, or details of any deployment.
