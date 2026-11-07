# skycue hardening

How the sanitizer, fuzz, static-analysis and soak runs are made, and what
they found and fixed. None of the instrumentation here is in the default
configure or any release; the shipped binaries are built exactly as before.
Run output (logs, summaries, soak CSVs) is not kept in the repository; each
part below is the procedure, plus the fixes it led to with their regression
tests.

## Skip rules and suppression formats

Every runner and every ctest here tells a missing tool from a failure:

| check | skips (exit 77, ctest "Skipped") when | the line printed |
|---|---|---|
| `run-sanitizers.sh tsan` | no clang++ | `SKIP: clang++ not found (gcc TSan does not run on every kernel)` |
| `run-fuzz.sh` (without `--bin-dir`) | no clang++; clang cannot link `-fsanitize=fuzzer` | `SKIP: clang++ not found (fuzz harnesses need clang)`, `SKIP: libFuzzer not available (<first error line>)` |
| ctest `fuzz_smoke` | `SKYCUE_FUZZ` off; the script missing | `SKIP: SKYCUE_FUZZ is OFF (...)`, `SKIP: scripts/dev/run-fuzz.sh not found` |
| `run-static.sh` | none of the selected tools found (one missing is `SKIP` in its count, the other still runs) | `SKIP: clang-tidy: clang-tidy or run-clang-tidy not found`, `SKIP: cppcheck: cppcheck not found`, then `static: clang-tidy=SKIP cppcheck=SKIP` |
| ctest `static_analysis` | `SKYCUE_STATIC_TESTS` off; no clang-tidy; the script missing | `SKYCUE_STATIC_TESTS is OFF`, `clang-tidy not found`, `scripts/dev/run-static.sh not found` |
| `soak.sh`, ctest `soak_short` | no `node`, a node without the built-in WebSocket (before 22), no `curl`; `SKYCUE_SOAK_TESTS` off | `SKIP: node not found`, `SKIP: node without WebSocket (need node >= 22)`, `SKIP: curl not found` |

`run-sanitizers.sh asan` does not skip: it falls back to g++. A skip is never
a pass: ctest reports it with `SKIP_RETURN_CODE 77`, and a runner's exit 77
is not 0.

Suppressions, each with its reason beside it. Today there are the four
cppcheck entries and 45 clang-tidy `NOLINT` comments in the source
(`git grep -c NOLINT | awk -F: '{s+=$NF} END{print s}'`), plus the
checks turned off in `.clang-tidy`; TSan, LSan and fuzzing need
none:

- TSan and LSan: there are none. A suppression file would use the
  sanitizers' own format (`race:<function or file>`, `leak:<function or
  library>`), one entry per line with a `# <reason>` line above it; the
  runner passes it at test time when it exists (see Sanitizer runs
  below), with no reconfigure.
- clang-tidy: inline `// NOLINTNEXTLINE(<check>): <reason>`; a check turned
  off in `.clang-tidy` has its reason there.
- cppcheck: `<id>:<file>` under a `# <reason>` line in
  `.cppcheck-suppressions`, no line number (below); no inline
  cppcheck suppressions.
- Fuzzing has no suppressions: a finding is fixed and becomes a regression
  test.

## Sanitizer runs

### Build options

`cmake/Hardening.cmake` adds two cache options to the skycue
subproject:

- `SKYCUE_SANITIZE`: `OFF` (default), `address,undefined` or `thread`.
  Anything else stops the configure with
  `SKYCUE_SANITIZE must be address,undefined, thread or OFF`. When set,
  libskycue, the daemon, the tools, the examples and the tests compile with
  `-fsanitize=<value> -fno-omit-frame-pointer -fno-sanitize-recover=all` and
  line tables only (`-gline-tables-only` with clang, `-g1` with gcc), and link
  with `-fsanitize=<value>`.
- `SKYCUE_TEST_SLOW_FACTOR`: empty (default) means 4 in a sanitizer or fuzz
  build and 1 otherwise. In an instrumented build, and in any build where
  it is set (a slow or emulated build machine), every test in
  `tests` and `examples` gets its ctest `TIMEOUT` (1500 s when it has none)
  multiplied by the factor and `SKYCUE_TEST_SLOW=<factor>` in its
  environment. The C++ tests scale their upper-bound waits with
  `slow()` from `tests/test_slow.hpp` (the shared `kWait`), and the
  Playwright config scales its `timeout` and `expect.timeout`. A check that
  something did *not* happen within a time is not scaled.

CMake sets no `*_OPTIONS` variable and no suppression file. The sanitizer
environment is exported by `scripts/dev/run-sanitizers.sh` at test time and ctest
passes it to every test, so a suppression file added later
(`tests/sanitizer/tsan.supp`, `tests/sanitizer/lsan.supp`, each
entry with a one-line reason) is used without reconfiguring.

### Running

```sh
scripts/dev/run-sanitizers.sh asan                 # ASan + UBSan, tree build-asan/
scripts/dev/run-sanitizers.sh tsan                 # TSan, tree build-tsan/ (clang only)
scripts/dev/run-sanitizers.sh asan -- -R '^inputs$' --repeat until-fail:5
scripts/dev/run-sanitizers.sh asan --canary        # prove child-process reports are caught
scripts/dev/run-sanitizers.sh asan --clean         # remove the tree after the run
```

Options: `--build-dir DIR`, `-j N` (default `nproc`), `--canary`, `--clean`,
and ctest arguments after `--`. `--clean` removes the tree only when it holds
a `CMakeCache.txt`; given a directory without one it refuses before doing
anything (exit 2), so a mistyped `--build-dir` is never deleted. The
compiler is `${CXX:-clang++}`; `asan` falls back to `g++` when there is no
clang++, and `tsan` without clang++ prints
`SKIP: clang++ not found (gcc TSan does not run on every kernel)` and exits
77 (gcc's TSan aborts at start-up on some kernels).

The script configures the tree once:

```sh
cmake -S . -B build-asan -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSKYCUE_BUILD_TESTS=ON \
  -DSKYCUE_UI_TESTS=ON -DSKYCUE_SANITIZE=address,undefined
```

then builds it, exports the environment below, empties the log directory
(`SKYCUE_SANITIZER_LOG_DIR`, which `Hardening.cmake` caches as
`<build>/sanitizer-logs`; the script reads it from `CMakeCache.txt`), runs
`ctest --test-dir <build> --output-on-failure -j N <CTEST_ARGS>`, prints every
file in `sanitizer-logs/` and fails if there is one. Every sanitizer writes
to that directory through `log_path`, so a report from a child process (a
`skycued` started by a shell test or by `test_web_server`, which only checks
its output) fails the run as well. The last line is
`sanitizers: <mode>: tests=<T> failed=<F> reports=<R>`; the exit status is 0
when clean, 1 on a failed test or any report, 77 when skipped.

The environment (`scripts/dev/run-sanitizers.sh --print-env MODE` prints it as
`export` lines, for running an instrumented ctest by hand). Each mode exports
only its own sanitizer variables and unsets the others' (the TSan runtime
also reads `UBSAN_OPTIONS`, and its `log_path` would win over
`TSAN_OPTIONS`'):

```sh
# asan
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:log_path=<build>/sanitizer-logs/asan
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:log_path=<build>/sanitizer-logs/ubsan
LSAN_OPTIONS=suppressions=tests/sanitizer/lsan.supp   # only when that file exists
# tsan
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:log_path=<build>/sanitizer-logs/tsan
#   plus :suppressions=tests/sanitizer/tsan.supp when that file exists
# both
SKYCUE_UI_SCREENSHOTS=<build>/ui-shots
SKYCUE_FISB_SCREENSHOTS=<build>/ui-shots
```

`detect_leaks=1` is written out although LeakSanitizer is already on with
ASan on x86-64 Linux: it states the intent, and it keeps leak checking on
where the platform default is off. A leak in a test or in a child `skycued`
is a finding; leak reports go through the same `log_path`, so a leak in a
child process fails the run too. A leak inside a system library, with no
skycue frame, would go in `lsan.supp` with a reason; none has been needed.

In an ASan + UBSan build both runtimes share one `log_path`, and the one from
`UBSAN_OPTIONS` wins: ASan's reports land in `ubsan.<pid>` files. The run
counts files, so this changes nothing but the name.

### The canary

`--canary` builds `sanitizer_canary` (`tests/sanitizer/canary.cpp`,
`EXCLUDE_FROM_ALL`) and runs it under the same environment. Under ASan +
UBSan its `main` forks two children, one per sanitizer, since
`halt_on_error` stops a child at its first report: one does a signed
overflow on a value derived from `argc` (UBSan), the other an out-of-bounds
heap read (ASan). Under TSan it forks one child in which two threads write
one variable without a lock. The parent exits 0 whatever happened. The
script passes (`canary: detected`) only when the log directory holds every
expected report, matched by text (`runtime error:` and
`ERROR: AddressSanitizer` for asan, `WARNING: ThreadSanitizer` for tsan).
With both children the UBSan report
(`signed integer overflow: 2147483647 + 1`) and the ASan
`heap-buffer-overflow` report reached `sanitizer-logs/` and the canary
passed.

### TSan

TSan is clang-only: gcc's TSan runtime aborts at start-up on some kernels
("unexpected memory mapping"), so `scripts/dev/run-sanitizers.sh tsan` uses
clang++ and skips (exit 77) without it.

```sh
scripts/dev/run-sanitizers.sh tsan --canary   # canary: detected (tsan.<pid>)
scripts/dev/run-sanitizers.sh tsan
```

- `inputs` (`edit_while_receiving_rebinds_same_port`): the UDP sender thread
  called `fixture()`, whose `BOOST_REQUIRE_MESSAGE` writes Boost.Test's
  checkpoint state while the main thread's assertions write it too;
  Boost.Test is not thread-safe. Fix: the fixture is read on the main thread
  before the sender starts. This also showed the TSan report landing in
  `ubsan.<pid>` (the TSan runtime reads `UBSAN_OPTIONS`'s `log_path`); the
  script now exports only the mode's own variables.
- `web_server` (`ws_snapshot_then_events`): the test wrote a text frame with
  `ws.write()` on the main thread while `WsClient`'s reader thread was inside
  `ws.read()` on the same Beast websocket stream, a race on the stream's
  state (`check_stop_now`). Timing-dependent. Fix: `WsClient::sendText`
  writes the masked frame to the socket itself, which is safe beside the
  reader.
- Both are test-side races; no daemon or library code raced. Each test
  reproduced its race under TSan before the fix and is the regression check
  (`-R '^inputs$'` and `-R '^web_server$'`, `--repeat until-fail:3`).
There are no TSan suppressions (`tsan.supp` does not exist).

## Fuzzing

### Build option and harnesses

`SKYCUE_FUZZ` (default `OFF`) builds the libFuzzer harnesses in
`tests/fuzz/`. It needs clang: with another compiler the configure stops
with `SKYCUE_FUZZ=ON needs clang (CMAKE_CXX_COMPILER_ID is <id>)`, and with
`SKYCUE_SANITIZE=thread` with
`SKYCUE_FUZZ=ON cannot be combined with SKYCUE_SANITIZE=thread`. When on,
everything in the tree compiles with
`-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer
-fno-sanitize-recover=all -gline-tables-only` and links with
`-fsanitize=address,undefined`, so the parsers are instrumented and the
ordinary tests still build and run in the fuzz tree (with the slow factor,
as in a sanitizer build). Only the harnesses link the libFuzzer driver.

`skycue_add_fuzzer(<name> SOURCES <files...> [LIBS <targets...>])`
(`cmake/Hardening.cmake`) adds the executable `fuzz_<name>` in
`<build>/fuzz/`, linked with `skycued_core` and `LIBS` and
`-fsanitize=fuzzer`, and appends `<name>` to the global property
`SKYCUE_FUZZ_TARGETS`; `tests/fuzz/CMakeLists.txt` also writes the names to
`<build>/fuzz/targets.txt` for the runner. Each harness calls a pure parser
entry point, never a socket or a file, except `ws`, which runs a real
WebServer on an ephemeral loopback port in its own process (below). A
harness that needs files makes them under `$SKYCUE_FUZZ_WORK/<target>/` (set
by the runner), or else under `std::filesystem::temp_directory_path()`.

`tests/fuzz/fuzz_util.hpp` (namespace `skycuefuzz`) holds the shared
pieces: `FUZZ_CHECK(cond)` (prints `FUZZ_CHECK failed: <cond> at
<file>:<line>` and aborts), `splitMode`, `chunked` (feeds a payload in chunks
of 1 to 64 bytes from a xorshift32 seeded with `seed | 1`), `validUtf8`
(RFC 3629), `quietLogs` (log level error) and `canary`.

**Mode byte.** A harness that drives more than one entry point takes the
first input byte as its mode: `splitMode(data, size, modes)` gives
`mode = uint8_t(data[0] - '0') % modes` and the rest as the payload (an empty
input is mode 0 with an empty payload). Seeds therefore start with an ASCII
digit, `0{...}` for mode 0. A single-mode harness (`readsb_line`) has no
mode byte: the whole input is the payload.

**Seeds** are cut from fixtures and test vectors, one input per file, named
`seed-<n>.<ext>`; they total well under 1 MB. Those cut from third-party
captures are not committed: `scripts/fixtures/gen-capture-seeds.py` generates them
from the fetched captures at fuzz time and `scripts/dev/run-fuzz.sh` copies them
into each corpus beside the committed seeds (a `SKIP: generated seeds` line
when the captures are not fetched). Every harness still has at least one
committed seed. Generated corpora, artefacts and work dirs stay in the build
tree; findings are copied to `tests/fuzz/findings/` (ignored by git except
`.gitkeep` and its `README.md`; the directory is never used as a corpus) and
become regression tests.

| Seeds | Cut from |
|---|---|
| `seeds/readsb_line/seed-1.jsonl` .. `seed-12.jsonl` | `tests/fixtures/readsb-sample.jsonl` lines 1-12 (one of each aircraft, with and without position, `alt_baro` "ground", a `~` TIS-B address) |
| `seeds/readsb_line/seed-13.jsonl` .. `seed-20.jsonl` | the same file, lines 1387-1394 (the last lines, including the two with the most keys) |
| `seeds/raw_formats/seed-1.txt` .. `seed-6.txt` (mode 0) | generated by `scripts/fixtures/gen-capture-seeds.py` at fuzz time from the fetched captures: `tests/fixtures/raw/uat-sample.txt.gz`, the first 6 lines |
| `seeds/raw_formats/seed-7.txt` .. `seed-12.txt` (mode 0) | generated by `scripts/fixtures/gen-capture-seeds.py` at fuzz time from the fetched captures: `tests/fixtures/raw/stratux-uat-2015.log.gz`, the first 6 non-blank lines (two `START,` headers, four offset lines) |
| `seeds/raw_formats/seed-13.txt` .. `seed-18.txt` (mode 2) | generated by `scripts/fixtures/gen-capture-seeds.py` at fuzz time from the fetched captures: `tests/fixtures/raw/modes-120s.csv.gz`, the first 6 lines |
| `seeds/raw_formats/seed-19.txt` .. `seed-21.txt` (mode 1) | the AVR vectors of `avr_input_lines` in `tests/test_raw_formats.cpp` (`@` with CR/LF, `*`, a Mode A/C line) |
| `seeds/raw_formats/seed-22.bin` .. `seed-24.bin` (mode 3) | the Beast streams of `splits_frames_with_escapes_at_any_chunking`, `skips_garbage_unknown_types_and_cut_frames` and `receiver_id_prefix_is_its_own_frame` in the same file |
| `seeds/recording/seed-1.jsonl` .. `seed-6.jsonl` (mode 0) | generated by `scripts/fixtures/gen-capture-seeds.py` at fuzz time from the fetched captures: `tests/fixtures/fisb/received-with-uplinks.jsonl` lines 1-6 (a `uat_uplink` record, five readsb lines) |
| `seeds/recording/seed-7.jsonl` .. `seed-10.jsonl` (mode 1) | the capture lines of `capture_line_round_trip` (two) and `capture_line_rejects_bad_hex_and_missing_keys` (the good line and the `serial` transport) in `tests/test_capture.cpp` |
| `seeds/fisb_decoder/seed-1.bin` .. `seed-6.bin` (mode 0, raw bytes) | generated by `scripts/fixtures/gen-capture-seeds.py` at fuzz time from the fetched captures: `tests/fixtures/fisb/tg30.978` lines 1, 5, 59, 15, 365 and 631 (product 413 text; AIRMET 11; G-AIRMET 14; NOTAM 8; NEXRAD 63 and 64; icing and turbulence 70, 71, 90, 91), the hex decoded; decoder byte `0` except seeds 2 and 5 (`1`, experimental) |
| `seeds/fisb_decoder/seed-7.txt` .. `seed-12.txt` (mode 1, the line) | generated like `seed-1` .. `seed-6`, from the same file, lines 82, 42, 46, 701, 777 and 301 (413 with NOTAM 8; SIGMET 12; SUA 13; NEXRAD 63 with turbulence 91; cloud tops 84; lightning 103), whole lines with their `rs=`/`rssi=`/`t=` fields; seeds 8 and 11 experimental |
| `seeds/fisb_decoder/seed-13.bin` (mode 0, raw bytes) | committed, hand-made from project data: the header bytes of `uplinkWith` in `tests/test_fisb_frame.cpp` and one information frame holding a product 413 APDU (T-opt 2, 11-01 17:53) with two DLAC reports (a METAR and a SPECI), padded to 432 bytes; the one committed seed of this harness |
| `seeds/iqgen_coding/seed-1.bin`, `seed-2.bin` (mode 0) | generated by `scripts/fixtures/gen-capture-seeds.py` at fuzz time from the fetched captures: the first 18 and 34 payload bytes of `tg30.978` lines 1 and 5, after a code byte (downlink short, downlink long) and a corruption index |
| `seeds/iqgen_coding/seed-3.bin` .. `seed-6.bin` (modes 1, 2) | the frames of `modes_crc_vectors` in `tests/test_iqgen.cpp`: `5D486258DD7987` and `8D4840D6202CC371C32CE0576098` as bytes (mode 1), `8D4840D6202CC371C32CE0576098` and `5f406a936f50f0` as text (mode 2) |
| `seeds/web/seed-1.txt` .. `seed-12.txt`, `seed-16.txt` .. `seed-25.txt` (mode 0) | requests on the routes of `docs/web-api.md`: GET `/api/state`, `/api/stats`, `/api/config`, `/api/filters/history`, `/api/fisb/text?type=METAR&station=KDCA`, `/index.html`, `/../etc/passwd`; PUT `/api/filters` with a JSON body; POST `/api/lists/allow`; POST `/api/inputs`; DELETE `/api/inputs/x`; a PUT with a foreign `Origin`; GET `/api/fisb/status`, `shapes`, `radar`, `raster`, `locations`, `/api/cues/clients`, `/api/tiles-available`, `/api/lists`, `/api/inputs/x/status`; POST `/api/filters/save-baseline` |
| `seeds/web/seed-26.json` (mode 1) | `tests/fixtures/pingstation/status.json` |
| `seeds/web/seed-14.txt`, `seed-15.bin` (mode 2) | a string with UTF-8, a tab, quotes and a backslash; the invalid sequences of `json_writer_output_is_valid_utf8` in `tests/test_readsb_json.cpp` with a NUL and 0x1f |
| `seeds/ws/seed-1.bin` .. `seed-5.bin` (raw frames) | a masked text frame "hi", a ping, a close (1000), a binary frame header claiming 2^40 bytes, a text message in two fragments |
| `seeds/pingstation/seed-8.json` (mode 0), `seed-9.json`, `seed-10.json` (mode 1) | `tests/fixtures/pingstation/traffic-1090.json`, `status.json`, `status-curly-quotes.json` (rejected), all generated by `gen.sh` with `skycue-pingsim`; `seed-2.json` (mode 0) is `uat-detail.json` |
| `seeds/pingstation/seed-11.bin` (mode 2) | the generated block `kSampleBlock` of `tests/test_cat021.cpp` (51 bytes), edition byte 2.5 |
| `seeds/pingstation/seed-12.bin` (mode 2), `seed-13.bin` (mode 3) | two vectors built on the generated block (`kSampleBlock`), edition byte 2.5: the datagram of `two_blocks_per_datagram` (the block, then a block of two records with address 000002); the stream of `framer_resync_after_garbage` (`00 15 ff 7e`, the block) with the CAT 48 block of `framer_skips_other_category_in_sync` and the block again, other categories accepted |
| `seeds/config_files/seed-1.conf` (mode 0) | `packaging/skycue.conf.example` with `RECEIVER_LAT=51.5`, `RECEIVER_LON=-0.12` (the values of `parses_example_file` in `tests/test_config.cpp`) |
| `seeds/config_files/seed-2.conf` (mode 0) | `tests/fixtures/config-copy.conf` |
| `seeds/config_files/seed-3.json` (mode 1) | the inputs file of `inputs_file_round_trip` in `tests/test_input_def.cpp` (`ps-north`, `ps-south`, `rb2`; readsb on, dump978 off) |
| `seeds/config_files/seed-4.json` (mode 2) | the overlay of `tests/test_runtime_config.cpp` (line 195) with a `min_elevation_deg`, without the unknown key |
| `seeds/config_files/seed-5.json` (mode 3) | a `fisb-locations.json` of three ids (`KPRG` from `tests/test_location_index.cpp`, a centroid with count 0) |
| `seeds/config_files/seed-6.csv` (mode 4) | five operator CSV lines from the same test: the header, a comment, `kprg`, `IND`, `BAD LINE` |
| `seeds/config_files/seed-7.txt` (mode 5) | a five-line allow list: a comment, an address, an N-number, `G-ABCD`, an address |

Each seed is the mode byte (none for `readsb_line`) and then the line without
its newline (`raw_formats/seed-19` keeps its CR/LF and `recording/seed-8`
its LF, as in the tests), or the stream's bytes; the `pingstation` JSON
seeds and `config_files/seed-1`, `seed-2` are the whole files, final newline
included, and the CAT021 seeds have their edition byte after the mode byte.

### Running

```sh
scripts/dev/run-fuzz.sh readsb_line                  # 600 s, tree build-fuzz/
scripts/dev/run-fuzz.sh all --seconds 60             # every harness, 60 s each
scripts/dev/run-fuzz.sh readsb_line --seconds 20 --canary --findings build/fuzz-findings
scripts/dev/run-fuzz.sh --clean                      # remove build-fuzz/ and its work dir
```

Options: `[TARGET|all]` (default `all`), `--seconds N` (default 600),
`--build-dir DIR` (default `<repo>/build-fuzz`), `--bin-dir DIR` (run the
`fuzz_*` executables already in DIR instead of building), `--work DIR`
(default `<build>/fuzz-work`), `--findings DIR` (default
`tests/fuzz/findings`), `--canary`, `--clean`.

Without `--bin-dir` the script needs clang++ (else
`SKIP: clang++ not found (fuzz harnesses need clang)`, exit 77) and a clang
that links `-fsanitize=fuzzer` (a one-line probe; else
`SKIP: libFuzzer not available (<first error line>)`, exit 77). It configures
the tree once:

```sh
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSKYCUE_FUZZ=ON
```

and builds only the `fuzz_*` targets. Every harness runs with
`SKYCUE_FUZZ_WORK=<work>`, `SKYCUE_TEST_SLOW=4` and the `ASAN_OPTIONS` and
`UBSAN_OPTIONS` of `scripts/dev/run-sanitizers.sh --print-env asan`, their
`log_path` under `<work>/sanitizer-logs/` (printed after a target that found
something). libFuzzer still reports on stderr and writes artefacts. Per
target:

```sh
fuzz_<t> -max_total_time=N -timeout=10 -rss_limit_mb=2048 -max_len=<L> \
  -print_final_stats=1 -artifact_prefix=<work>/artifacts/<t>/ <work>/corpus/<t>
```

with `L` 65600 for `web`, `pingstation` and `config_files` and 4096 for the
others. The corpus gets the target's seeds when it is empty and is kept
between runs; the artefact directory is emptied first. Each artefact is
copied to `<findings>/<t>-<name>`, libFuzzer's output goes to
`<work>/logs/<t>.log`, and the script prints

```
fuzz: <t> seconds=N execs=<stat::number_of_executed_units> corpus=<files> findings=<artefacts>
```

and appends the same fields to `<work>/summary.tsv`. It exits 1 if any
target had a finding (or a harness failed without one), else 0.

`--canary` runs on a scratch corpus (`<work>/canary-corpus/<t>`: the seeds
plus one starting `skycue-canary`) with `SKYCUE_FUZZ_CANARY=1`, on which
every harness aborts: the run must end with `findings=1` and exit 1. With
`--clean` the work dir and (unless `--bin-dir` was given) the build tree are
removed after the run, or at once when no target is named; a build dir
without a `CMakeCache.txt` is refused.

`fuzz_smoke` (ctest): in a default build it is
`SKIP: SKYCUE_FUZZ is OFF (fuzz harnesses build with clang only)`, and in a
vendored tree without the script `SKIP: scripts/dev/run-fuzz.sh not found`. In a
fuzz tree the fixture test `fuzz_build` builds every `fuzz_<name>` and
`fuzz_smoke` runs `run-fuzz.sh all --seconds 5 --bin-dir <build>/fuzz` with
its work and findings under `<build>/tests/fuzz-smoke/`.

### Adding a harness for a new parser

A new parser (the future frame decoder among them) brings its own harness
in the same change:

1. `tests/fuzz/fuzz_<name>.cpp`: `LLVMFuzzerTestOneInput` calls
   `skycuefuzz::canary(data, size)` first and `quietLogs()`, then the pure
   entry point of the parser. With more than one entry point, the first byte
   picks it (`splitMode`); a byte stream is fed through `chunked` so every
   split point is tried. Check the parser's documented invariants with
   `FUZZ_CHECK` (a round trip through its writer, ranges, both-or-neither
   fields), not only "does not crash". Catch only the exceptions the parser
   documents. No socket, no fixed path: files go under
   `$SKYCUE_FUZZ_WORK/<name>/`.
2. `tests/fuzz/CMakeLists.txt`: `skycue_add_fuzzer(<name> SOURCES
   fuzz_<name>.cpp [LIBS ...])`. The runner and `fuzz_smoke` pick it up from
   `targets.txt`; a larger `-max_len` than 4096 is a case in
   `scripts/dev/run-fuzz.sh`.
3. `tests/fuzz/seeds/<name>/seed-<n>.<ext>`: cut from fixtures or test
   vectors already in the repo, mode byte first, and a row in the seeds
   table above saying where each came from. Keep `tests/fuzz/seeds` under
   1 MiB in all (`du -sb tests/fuzz/seeds`).
4. Prove it: `scripts/dev/run-fuzz.sh <name> --seconds 20 --canary --findings
   <scratch dir>` must end with `findings=1`, then
   `scripts/dev/run-fuzz.sh <name>` (600 s) with `findings=0`.
5. Each finding: a regression test in the parser's own test file, named
   `fuzz_<name>_<what>`, red before the fix; the fix; then the 600 s run
   again on the kept corpus. Record the fix and its test here.

### Harnesses and the fixes they led to

| Target | Entry point | Fixes (test) |
|---|---|---|
| `readsb_line` | `parseReadsbLine` (`skycued/src/readsb_json_source.hpp`) | `"now"` up to 1e11 s overflowed the int64 nanosecond conversion (UBSan: `1.79355e+19 is outside the range of representable values of type 'long'`); the bound is now 9e9 s (`fuzz_readsb_line_now_overflow` in `tests/test_readsb_json.cpp`) |
| `raw_formats` | `parseUatLine`, `parseAvrLine`, `parseCsvMessage`, `beast::FrameSplitter` (`skycued/src/raw_formats.hpp`) | `parseCsvMessage` multiplied the integer digits into an int64_t before checking for more than 10 (UBSan: `1783963430000000053 * 10 cannot be represented in type 'int64_t'`), and 10 digits overflowed `secs * 1e9`; the digit limit is checked first and seconds are at most 9223372035 (`fuzz_raw_formats_csv_seconds_overflow` in `tests/test_raw_formats.cpp`). `avrLine` wrapped `epoch_ns * 3` in uint64_t past the year 2165 (the harness's AVR round-trip check, on `8888888889,5D4D032CB5BC2B`); it now computes `3 * (ns / 250) + 3 * (ns % 250) / 250` (`fuzz_raw_formats_avr_ticks_wrap`) |
| `recording` | `parseRecordingLine` (`skycued/src/recording_reader.hpp`) with `replayDelay` (`skycued/src/replay_source.hpp`); `parseCaptureLine`, `captureLineJson` (`skycued/src/asterix/capture_line.hpp`) | ReplaySource's pacing delay was clamped above only, so a record before the first at a small speed overflowed the conversion to steady_clock (UBSan: `-3.71505e+23 is outside the range of representable values of type 'long'`); `replayDelay` clamps to [0, 1e9 s] NaN-proof (`fuzz_recording_replay_delay_overflow` in `tests/test_recorder.cpp`). `parseIso8601Ms` took any 4-digit year and overflowed the milliseconds to nanoseconds conversion outside 1677-2262 (UBSan: `-30173607899877 * 1000000 cannot be represented in type 'long'`, a capture line dated 1013); such times are rejected (`fuzz_recording_iso8601_out_of_range` in `tests/test_recorder.cpp`, `fuzz_recording_capture_time_out_of_range` in `tests/test_capture.cpp`). In review: the replay offsets, gaps, span and loop shifts were int64 subtractions that overflowed for a 1677 and a 2026 record; `replayOffset` and the loop shift saturate (`replay_offset_saturates_across_the_clock_range` in `tests/test_recorder.cpp`) |
| `fisb_decoder` | `fisb::FisbDecoder::feed`, `sweep` (`skycued/src/fisb/fisb_decoder.hpp`), products through `textRecordJson`, `twgoRecordJson`, `shapeJson` (`skycued/src/fisb/fisb_json.hpp`) | none needed |
| `web` | `webapi::makeApiRequest` + `handleApi` on a stubbed daemon, `staticPath` (`skycued/src/web_api.hpp`); `JsonObject`, `JsonWriter` (`skycued/src/json_lite.hpp`) | none needed |
| `ws` | the WebSocket inbound path of `WebServer` (`skycued/src/web_server.cpp` `WsSession::read`), over loopback | none in the server: both were `connect` in the harness failing with EINTR (libFuzzer's timers); the harness now waits for an interrupted connect and retries poll/send/recv |
| `iqgen_coding` | `iqgen::ReedSolomon` `parity`/`syndromes` with the UAT codes, `uatEncode`, `uatFrameBits`, `modesCrcResidual`, `parseHex`, `toHex` (`tools/iqgen/`) | none needed |
| `pingstation` | `parsePingDatagram`, `parsePingStatusDocument` (`skycued/src/inputs/ping_json.hpp`); `asterix::splitBlocks`, `decodeBlock` (`skycued/src/asterix/cat021.hpp`), `Cat021StreamFramer` (`cat021_framer.hpp`) | `parseDeviceTime` took any 4-digit year and overflowed the seconds to nanoseconds conversion (UBSan: `65479180292 * 1000000000 cannot be represented in type 'int64_t'`, a `timeStamp` in 4044); the year must be 1678 to 2261 (`fuzz_pingstation_device_time_out_of_range` in `tests/test_pingstation.cpp`). In review: `checkDeviceTime`'s `t - arrival` still overflowed for a 1678-1733 device time against a present-day arrival; the GPS offset and the difference are now overflow-checked and such an offset is rejected, computed in double (`fuzz_pingstation_clock_check_far_apart`, red under UBSan on the old code). A 120 s run on the committed seeds after the fix: 900,363 execs, corpus 1055, `findings=0`. The framer's buffer was already bounded (`framer_buffer_bounded_on_garbage` in `tests/test_cat021.cpp`) |
| `config_files` | `parseConfigText` (`skycued/src/config.hpp`), `parseInputsFile`/`inputsFileJson` (`inputs/inputs_file.hpp`), `parseOverlay`/`overlayJson` (`runtime_config.hpp`), `fisb::LocationIndex::loadLearned`/`learnedJson`/`loadOperatorCsv` (`fisb/location_index.hpp`), `IdList::parse` (`filters.hpp`) | none needed |

`readsb_line` harness invariants: when the line parses, `raw_line` equals the
input and `lat`/`lon` are both set or neither.

`raw_formats` invariants: a UAT line's text starts with `-` or `+`, has no
line end, and has an epoch or a stratux offset, not both; an AVR line's hex is
14 or 28 uppercase digits and its ticks fit 48 bits; a CSV line's hex is 4, 14
or 28 uppercase digits, `epoch_ns >= 0`, and a Mode S one survives
`parseAvrLine(avrLine(...))` with the exact ticks; every Beast frame has
`body.size() == bodyLength(type)`, starts with 0x1a and ends within the bytes
fed, and `skippedBytes() <= fed() == payload size`.

`recording` invariants: a parsed line has `lat`/`lon` both set or neither, and
`replayDelay` of its time against a 2026-11-02 start at speeds 1e-6 to 1e6 is
within [0, 1e9 s]; a parsed capture line comes back with equal `t`, `input`,
`transport` and `bytes` through `captureLineJson` and `parseCaptureLine`.

`fisb_decoder` input: the mode byte, a byte whose low bit picks the normal or
the experimental decoder (one static decoder each, so segment reassembly
state builds up across inputs; reference time 2020-06-01T00:00:00Z), then the
payload, fed as `"+" + lowercase hex + ";"` (mode 0) or as the line itself
(mode 1). Every 256 inputs both decoders are swept at the reference time plus
61 min. Invariants: every text record's `textRecordJson` and every TWGO
half's `twgoRecordJson` (the record the store would build from it) is UTF-8
and parses as a JSON object, every `shapeJson` is UTF-8, every TWGO shape
point and global block NW corner is within |lat| <= 90, |lon| <= 180; `feed`
does not throw (no try/catch in the harness).

`iqgen_coding` invariants: mode 0, for the UAT code picked by the payload's
first byte (`uatCode`: downlink short, downlink long, uplink block), k =
min(size, 255 - nroots) data bytes plus their `parity` have all `syndromes`
0, and with one byte changed (index from the second byte) some syndrome is
not 0; a data part of 18, 34 or 432 bytes gives `uatFrameBits` of 36 + 8 per
`uatEncode` byte, each 0 or 1, half of `uatFrameSamples`. Mode 1: a 7- or
14-byte frame with its last 3 bytes set to `modesCrcResidual` of it with them
zeroed has residual 0. Mode 2: when `parseHex` succeeds, `toHex(out, false)`
is the lowercased payload; when it fails, `out` is unchanged.

`web` input: the mode byte, then the payload. Mode 0: raw request bytes
through Beast's `request_parser<string_body>` (body limit 64 KiB), the
request built with `makeApiRequest` as `WebServer` does. An `/api` path goes
to `handleApi` on the stub of `tests/fuzz/api_stub.hpp` (an Engine with no
recorders, never started, an InputManager in replay mode so every
`/api/inputs` write is a 409 and no listener opens, a Forwarder never
started, a RuntimeConfig on its own `skycue.conf` under
`$SKYCUE_FUZZ_WORK/web/`, a DaemonInfo; the io_context never runs); after
any method but GET/HEAD the stub's files are removed and it is built again,
so state does not build up across inputs. Invariants: `handleApi` does not
throw, its status is 200-499 (the stub's directory is writable, so no 5xx is
legitimate) and a JSON reply is valid UTF-8; for any other path a
`staticPath` result starts with `/`, has no empty, `.` or `..` segment and no
NUL or backslash. Mode 1: `JsonObject::parse` and every getter on every key,
into objects and object arrays to depth 3, without throwing. Mode 2:
`JsonWriter().add("k", payload)` is valid UTF-8, parses back, and gives the
payload itself when the payload is valid UTF-8.

`ws`: `LLVMFuzzerInitialize` starts a `WebServer` (`127.0.0.1`, port 0)
over the same stub (`$SKYCUE_FUZZ_WORK/ws/`) on a background thread. Each
input: connect, a fixed upgrade request, the 101 read back, the input written
raw, a half-close and a drain until the server closes (bounded by
`slow(2s)`). Invariant: a fresh connection's `GET /api/stats` answers 200
within `slow(2s)`; ASan's `-rss_limit_mb=2048` would catch an allocation
sized from a hostile frame length. About 1,000 execs/s (two TCP connections
each).

`pingstation` input: the mode byte, then the payload. Mode 0:
`parsePingDatagram` (arrival 2026-11-02T10:15:00Z, UTC time mode); every raw
frame's hex is lowercase and 7 or 14 bytes for `adsb_1090`, 18 or 34 for
`uat_978`; every observation has `lat`/`lon` both or neither, and the first
carries the datagram as `raw_line`, the others none; a status `version` is
empty or three integers joined by dots. Mode 1: `parsePingStatusDocument`,
the same `version` rule. Mode 2: the first payload byte gives the edition
(`% 6`), the rest goes to `splitBlocks`: the spans are in order, do not
overlap, lie inside the buffer, have `size >= 3`, `cat` equal to their first
byte and LEN equal to their size; `decodeBlock` runs on every CAT 21 span.
Mode 3: the first byte gives the edition (`(b & 0x7f) % 6`) and
`accept_other_categories` (`b & 0x80`), the rest feeds a
`Cat021StreamFramer` through `chunked` (seeded by its first 4 bytes); after
each chunk `buffered() <= 65535`, and every block out has LEN equal to its
size, CAT 21 (unless other categories are accepted) and decodes without
error. `framer_buffer_bounded_on_garbage` (`tests/test_cat021.cpp`) pins the
bound on 1 MiB of CAT 21 headers with LEN 0xffff: the framer already held at
most one block (a waiting candidate has fewer bytes than its LEN), so no fix
was needed.

`config_files` input: the mode byte, then the file's text, on the parsers
alone (no file is read or written, so no work dir is used). Mode 0:
`parseConfigText`; 1: `parseInputsFile`, and a file that parses gives equal
`inputs` (`InputDef::operator==`) and `builtin_enabled` through
`inputsFileJson` and `parseInputsFile` again; 2: `parseOverlay`, and
`parseOverlay(overlayJson(o)) == o`; 3: `LocationIndex::loadLearned` on a
fresh index, and after a load that succeeds, `loadLearned(learnedJson())`
leaves `learnedJson()` unchanged; 4: `loadOperatorCsv` returns at most the
number of lines and `operatorCount()` is at most that; 5: `IdList::parse`.
Each mode catches only the exception its parser documents (`ConfigError`,
`InputDefError`, `std::runtime_error`); `loadOperatorCsv` documents none.

Other fixes the fuzzing led to: the JSON escaper copied invalid UTF-8
verbatim (`json_writer_output_is_valid_utf8` in `tests/test_readsb_json.cpp`
and `capture_line_json_is_valid_utf8` in `tests/test_capture.cpp`);
`parseCaptureLine` took an `input` holding invalid UTF-8 (overlong `C0 A3`,
which PropertyTree passes) that `captureLineJson` writes as U+FFFD, so such a
line is now refused (`fuzz_recording_capture_input_invalid_utf8` in
`tests/test_capture.cpp`). Two early `ws` stops were the harness's own
`connect` being interrupted (`EINTR`), now retried.

## Static analysis

### Running

```sh
scripts/dev/run-static.sh                  # clang-tidy and cppcheck, tree build-static/
scripts/dev/run-static.sh --tidy-only      # clang-tidy alone (what ctest runs)
scripts/dev/run-static.sh --cppcheck-only -j 8 --build-dir /path/to/tree
```

The script configures skycue (no build: nothing is generated) with clang++
and `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, and analyses the translation units in
`compile_commands.json` under `skycued`, `libskycue`, `net/src` and
`tools/`; `tests/` (with `tests/fuzz/`) is not analysed. clang-tidy runs
through `run-clang-tidy` with `.clang-tidy`; cppcheck runs with
`--enable=warning,portability --std=c++17` and
`.cppcheck-suppressions`. Each finding is printed once (a header's
finding is not repeated per translation unit), and the last line is

    static: clang-tidy=<warnings|SKIP> cppcheck=<warnings|SKIP>

Exit 0 when both counts are 0, 1 on any warning (or a failed configure), 2
on a usage error, 77 when none of the selected tools is installed; a missing
tool is SKIP while the other still runs. The full logs are
`<tree>/clang-tidy.log` and `<tree>/cppcheck.log`. The tree is the
script's own: a `--build-dir` that already holds another build (a
`CMakeCache.txt` without the `.skycue-static-tree` marker the script writes)
is refused with exit 1, never reconfigured.

`SKYCUE_STATIC_TESTS=ON` (default `OFF`) registers the ctest
`static_analysis`: `run-static.sh --tidy-only` in
`<build>/tests/static-analysis`, timeout 1800 s. It is skipped with a reason
when the option is off, when clang-tidy is not found or when the script is
missing.

### Checks

`bugprone-*`, `cert-*`, `performance-*`, and three readability checks that
cannot change behaviour (`readability-misleading-indentation`,
`readability-redundant-string-cstr`, `readability-container-size-empty`).
`modernize-*` and the rest of `readability-*` are off. Turned off, each with
its reason in `.clang-tidy`:

- `bugprone-easily-swappable-parameters`: a style rule that flags every
  function taking two parameters of one type.
- `bugprone-narrowing-conversions`: `-Wconversion` already covers it.
- `bugprone-unchecked-optional-access`: its dataflow cannot see the
  invariants the parsers rely on (all of its findings were false
  positives, see below).
- `performance-enum-size`: a smaller underlying type changes libskycue's ABI.
- `performance-inefficient-string-concatenation`: only error-message
  building before a throw or a warning was flagged.

Options: `boost::system::error_code` is dropped from
`bugprone-unused-return-value` / `cert-err33-c` `CheckedReturnTypes` (asio's
`ec` overloads return the error they also store in `ec`, which the code tests
or, when closing, ignores on purpose), and
`bugprone-implicit-widening-of-multiplication-result` ignores constant
expressions such as `64 * 1024`.

Suppressions: inline `// NOLINTNEXTLINE(<check>): <reason>` for clang-tidy,
and an `<id>:<file>` entry under a `# <reason>` line in
`.cppcheck-suppressions` for cppcheck (no inline cppcheck
suppressions). The four cppcheck entries are structural false positives,
so they name the file but no line, and do not go stale when the file is
edited.

### Triage

How the clang-tidy and cppcheck findings were settled:

| check | outcome |
|---|---|
| `bugprone-unused-return-value` / `cert-err33-c`, asio `ec` overloads | option: `error_code` return types not checked (above) |
| `cert-err33-c`, C library calls | 1 fixed (`strftime` length used); 36 NOLINT: 24 `snprintf` and 1 `vsnprintf` into buffers sized for their format, `signal`/`setlocale` with constant arguments, `fwrite`/`fflush` to stdout or stderr, a best-effort `remove` |
| `bugprone-unchecked-optional-access` | check off; all false positives: `BitReader` values read before one `overrun()` test, pairs checked together (`first`/`last`, `lat`/`lon`, `sac`/`sic`), pointer-to-member optionals, `emplace()` then use |
| `performance-enum-size` | check off (ABI) |
| `bugprone-implicit-widening-of-multiplication-result` | 13 constants (option); 2 fixed: `device_time.cpp` hours and minutes multiplied in `int64_t` |
| `performance-unnecessary-value-param` | 12 fixed (resolver results and strings by `const&`); 1 NOLINT, `ObservationSource::onUplink`'s default no-op; 2 more found once the listener `Impl`s took `const&`, fixed |
| `performance-inefficient-vector-operation` | fixed: `reserve()` |
| `bugprone-exception-escape` | 3 fixed: `skycued`, `skycue-rawrec` and `skycue-pingsim` `main()` catch what escapes (`io.run()` rethrows a handler's exception) and exit 1 with a message instead of `terminate()`; 5 NOLINT, the destructors of `Client`, `Forwarder`, `InputManager`, `StatusPoller` and `RuntimeConfig` (a throw from asio's cancel/close or a join there is a broken process, and terminate is right) |
| `bugprone-assignment-in-if-condition` | fixed: `parseHostPort` assigned before the test |
| `performance-inefficient-string-concatenation` | check off (error messages) |
| `bugprone-optional-value-conversion` | fixed: the optional assigned, not `*opt` |
| `performance-move-const-arg` | fixed: `ptree` has no move constructor, so `JsonObject` takes `const ptree&` or swaps a `ptree&&` in (one copy fewer per parse); a trivially copyable `std::move` dropped |
| `cert-dcl50-cpp` | NOLINT: printf-style wrappers whose format attribute has the compiler check each call |
| `performance-no-automatic-move` | fixed (`const` dropped) |
| `cert-err58-cpp` | fixed: `kRestartKeys` is `constexpr` |
| `bugprone-macro-parentheses` | NOLINT: `SKYCUED_LOG`'s `expr` is a `<<` chain |
| `bugprone-branch-clone` | fixed: the two hex-digit branches merged |

cppcheck, all four suppressed as false positives (reasons in the file):
`legacyUninitvar` `fisb/apdu.cpp:12` (the reference target is written before
it is read), `uninitvar` `fisb/segment_assembler.cpp:34` (`Set::filled` has a
default member initializer), `knownConditionTrueFalse` x2
`tools/skycue-pingsim.cpp:435` (program_options writes `sac` and `sic`
through pointers), `invalidFunctionArg` `tools/iqgen/iq_writer.cpp:91` (the
loop guarantees a non-empty buffer).

The checks turned off (`bugprone-unchecked-optional-access`,
`performance-enum-size`, `performance-inefficient-string-concatenation`) are
the accepted findings, for the reasons above.

### -Werror

`SKYCUE_WERROR=ON` (skycue's own option) adds `-Werror` to every skycue
target. Whole tree (daemon, tools, tests, examples), Release:

- gcc 11.5: two `-Wstringop-overread` false positives in test code
  (`test_cat021.cpp` `block()`, `test_inputs.cpp` `rawBlock()`: the memcpy
  inlined from `vector::insert`, on the path where the inserted range is
  empty). Each `insert` has a local `#pragma GCC diagnostic` push/ignored/pop,
  for gcc 11 and later only, with a comment naming the false positive. Then
  clean.
- clang 21: `-Wunused-private-field` (`Engine::io_`, never read: removed)
  and `-Wc++20-extensions` (a lambda in `InputManager::applyFile` captured
  a structured binding, which C++17 does not allow: named references
  instead). Then clean.
- gcc 8.5 (RHEL 8 compatible, Boost 1.66): proved on the sweep's final head by
  a clean-room build with `SKYCUE_WERROR=ON` (0 compiler
  warnings), and gcc 11 again on RHEL 9 compatible; both plain and with
  `SKYCUE_WERROR=ON`.

`SKYCUE_WERROR` (default `OFF`) turns compiler warnings into errors; the
default argument list is unchanged.

## The net library

`net/` (skycue::net, see `net/README.md`) is part of these runs. Its tests
(`net_endpoint`, `net_framer`, `net_backoff`, `net_dialer`, `net_listener`,
`net_udp`, `net_fanout`) are ordinary ctests, so every sanitizer and fuzz
tree runs them with the library compiled with the same instrumentation and
the slow factor applied; `ctest -R '^net_'` runs just those. The line framer
has the libFuzzer harness `net_line_framer` (`tests/fuzz/fuzz_net_line_framer.cpp`),
run like the others with `scripts/dev/run-fuzz.sh net_line_framer`. The
static-analysis run (`scripts/dev/run-static.sh`) analyses `net/src` with the
rest of the library and daemon sources; `net/tests` is not analysed, like
`tests/`.

## The 1090 decoder

The frame decoder (`skycued/src/decode/`) has the libFuzzer harness
`decode_1090` (`tests/fuzz/fuzz_decode_1090.cpp`): the input is cut into
7 and 14 byte frames by a length byte and fed to a `ModesDecoder` with a
64-key table, so eviction runs too; it must not throw, every frame must be
counted, and `lat`/`lon` of an observation are both set or neither. Seed:
`seeds/decode_1090/seed-1.bin` (one DF17 identification frame and one DF11
frame, each after its length byte). Run it like the others with
`scripts/dev/run-fuzz.sh decode_1090`.

The robustness test is an ordinary ctest, so every sanitizer and fuzz tree
runs it: `ctest -R '^decode_robustness$'` (10 MB of random bytes, single and
multiple bit flips against a clean synthetic flight, key table limits, wrong
frame lengths; fixed PRNG seeds). `ctest -R '^(decode_|coding)'` runs all the
decoder tests.

## Soak

### Running

```sh
cmake --install build --prefix "$PWD/stage-test"     # this tree's own stage
tests/soak/soak.sh --stage "$PWD/stage-test" --out build-soak/run-4h
tests/soak/soak.sh --skycued BIN --pingsim BIN --dump BIN --www DIR \
    --minutes 2 --interval 5 --growth 1.0 --out DIR   # what soak_short runs
```

Options: `--minutes` (default 240), `--interval` seconds between samples
(60), `--out` (`./soak-<UTC timestamp>`), `--fixtures`
(`tests/fixtures`), `--growth` (0.05) and `--fd-tolerance` (2), the
last two passed to `soak_eval.py` unchanged. Exit 0 PASS, 1 FAIL, 77 SKIP
(no `node`, a `node` without the built-in WebSocket, or no `curl`), 2 on a
usage error. `--stage` is the tree's own installed prefix, never another
checkout's stage. At the default `--growth 0.05` a run shorter than about
40 minutes fails on instance A: its FIS-B store is still filling during the
first replays (about 10 minutes), which then falls inside the window.

Two `skycued` instances run, each with its own conf (the test receiver
position, ephemeral web and cue ports, never the demo's) and log:

- **A** replays `fixtures/fisb/received-tg30.jsonl` with `--replay-loop` at
  real time: FIS-B uplinks (METARs, TAFs, NEXRAD, rasters) through the store
  and decoder, the file restarting every 7.6 minutes. Its `observations` and
  `tracks_total` columns stay 0; the script prints its FIS-B uplink count at
  the end.
- **B** has no readsb or dump978 and one `pingstation_json` UDP input, fed
  by `skycue-pingsim --as json --loop --speed 5` from
  `fixtures/readsb-sample.jsonl` (tracks).

Each instance has one WebSocket client (`tests/soak/ws_client.mjs`, node 22's
built-in WebSocket: it counts messages and reconnects on a close, logging the
reconnect) and one cue TCP client (`skycue-dump --tcp` on the cue port read
back from the daemon's `forwarder: listening on TCP port` line; an
unreadable port fails the run before any client starts). Every
`--interval` the script writes one row per instance to `soak.csv`:

    t_s,instance,pid,rss_kb,vsz_kb,fds,threads,observations,web_clients,tcp_clients,tracks_total

from `/proc/<pid>/status`, `/proc/<pid>/fd` and `GET /api/stats`. A daemon
or a client that exits during the run is a FAIL (`FAIL: skycued A exited
(status 137)` and the log's last 20 lines). Everything started is stopped by
its recorded pid (`<out>/*.pid`) on every exit path: TERM, a bounded wait of
3 s, then KILL. `--out` keeps `A.log`, `B.log`, the client logs,
`soak.csv` and `verdict.txt`.

### The pass rule

`tests/soak/soak_eval.py CSV [--growth 0.05] [--fd-tolerance 2]`, per
instance over its samples ordered by `t_s`: the window is the samples with
`t_s >= 0.25 * t_max` (the first quarter is warm-up); the reference is the
median RSS of the first 3 window samples and the end the median of the last
3; the instance passes when `(end - ref) / ref < growth` and every window
sample's fd count is within `fd-tolerance` of the first window sample's.
Fewer than 6 window samples is a FAIL (`too few samples`). Exit 0 with a
`PASS <instance>: ...` line each, 1 with `FAIL <instance>: <reason>` and
the series. `soak_eval.py --self-test` checks the rule on synthetic series
(flat, a 20 % warm-up, a 6 % rise, a 4.9 % rise, an fd step of 3, 4
samples); it is the always-on ctest `soak_eval`.

`SKYCUE_SOAK_TESTS=ON` (default `OFF`) registers the ctest `soak_short`:
2 minutes at a 5 s interval, `--growth 1.0` (two minutes say nothing about a
leak: the test proves the CSV is written and the rule evaluates), timeout
400 s. Off, it is skipped with the reason.
