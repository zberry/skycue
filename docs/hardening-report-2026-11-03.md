# skycue hardening sweep: report (2026-11-03)

The sweep added sanitizer builds, libFuzzer harnesses, static analysis, a
soak test and closed the parked minors from the merged branches. It added no
features. The procedure for every part, with options, skip rules and
suppression formats, is [hardening.md](hardening.md); this report is the
result. All runs were on the development machine; every instrumented tree
was a separate build directory, and the default build configuration is
unchanged. The fixes do change behaviour in places; they are listed under
[Behaviour changes](#behaviour-changes).

Each fix names its regression test (file under `tests/` unless
another path is given, and case) and the subject of the commit that made it.

## Final reruns

On the finished branch, each from a fresh build tree. The sanitizer runs,
`run-static.sh`, the default suite and the superbuild scripts were run again
after the capture fix the fuzz rerun led to, with the same result lines.

| run | result |
|---|---|
| `scripts/run-sanitizers.sh asan` | `sanitizers: asan: tests=51 failed=0 reports=0` |
| `scripts/run-sanitizers.sh tsan` | `sanitizers: tsan: tests=51 failed=0 reports=0` |
| `scripts/run-fuzz.sh all --seconds 600` | nine `fuzz:` lines, eight with `findings=0`; `recording` had `findings=1` (a new finding, fixed below), and its 600 s rerun after the fix: `fuzz: recording seconds=600 execs=3610645 corpus=2461 findings=0` |
| `scripts/run-static.sh` | `static: clang-tidy=0 cppcheck=0` |
| `ctest --test-dir build-fuzz -R fuzz_smoke` | `100% tests passed, 0 tests failed out of 2` (`fuzz_build`, `fuzz_smoke`: nine targets, 5 s each, `findings=0`) |
| default suite, `ctest -j8` (with `SKYCUE_SOAK_TESTS=ON`) | `100% tests passed, 0 tests failed out of 51` (`soak_short` passed in 120 s, `PASS A: rss 8708 -> 9320 kB (+7.0%), fds 13..13, 19 window samples` and `PASS B: rss 7644 -> 7648 kB (+0.1%), fds 14..14, 19 window samples` at `--growth 1.0`, the last run, on the final code; `ui_playwright` passed; `fuzz_smoke` and `static_analysis` skipped, their options off in a default tree) |
| `SKYCUE_STAGE=<the branch's stage-test> tests/superbuild/run.sh` | exit 0, every script PASS; `test_iq_replay.sh` SKIP (readsb is not in a skycue-only stage) |
| Rocky 8 container (gcc 8.5, Boost 1.66): `scripts/verify-offline-build.sh 8 <bundle>`, plain and with `--werror` (`SKYCUE_WERROR=ON`), on the final head | both ended `verify: OK rocky8`; ctest 50/50 passed, 3 skipped as exit 77 (`fuzz_smoke`, `static_analysis`, `soak_short`); 0 compiler warnings in the tree; no `-fsanitize` and no `SKYCUE_FUZZ` in the configure; `test_receivers.sh` ok, `test_make_locations` PASS, the start-script test ran; in place and relocated |
| Rocky 9 container (gcc 11, Boost 1.75): the same procedure, plain and with `--werror` | both ended `verify: OK rocky9`, with the same counts and checks |

`du -sb fuzz/seeds`: 82876 bytes (limit 1048576).

## Behaviour changes

What an operator or a tool user can see, old behaviour then new:

| change | before | now |
|---|---|---|
| readsb JSON `"now"` | accepted up to 1e11 s (overflowed above 9e9 s); NaN passed | refused above 9e9 s (year 2255) and when NaN |
| CSV raw-format seconds | 10-digit epochs up to 9999999999 accepted (overflowed `secs * 1e9`) | refused above 9223372035 |
| ISO 8601 times in recordings and capture lines | any 4-digit year (overflowed outside the clock's range) | outside 1677-09-21..2262-04-11 the line is ignored |
| pingStation `timeStamp` year outside 1678..2261 | overflowed (undefined behaviour) | counts as no device time: the arrival time is used, no clock reject counted |
| capture line with an `input` that is not valid UTF-8 | parsed, but did not round-trip | the line is refused (replay, `skycue-dump --capture`, `skycue-pingsim --capture`) |
| invalid UTF-8 in any JSON output (API, WebSocket, recordings, capture lines) | written verbatim (invalid JSON text) | each invalid byte written as U+FFFD |
| `skycue-dump --json` strings | bytes from 0x7f escaped as Latin-1 `\u00XX` (valid non-ASCII UTF-8 came out as mojibake) | valid UTF-8 printed as it is; each invalid byte as U+FFFD |
| input `name` with a C1 control or invalid UTF-8, through the API | accepted | refused with `400` |
| such a `name` already in the inputs file | loaded | repaired at load (invalid bytes to U+FFFD, controls dropped, cut to 64 characters), logged at ERROR with the input id; every input still loads, the file is not rejected |
| a FIFO or other non-regular file in the www dir | the io thread hung | a static request gets `404`; `tiles/` and `overlays/` `meta.json` count as absent |
| `*/meta.json` caching | `Cache-Control: max-age=86400` under `/tiles/` | `no-cache` |
| an exception escaping `main()` of `skycued`, `skycue-rawrec`, `skycue-pingsim` | `terminate()`: SIGABRT and a core | exit status 1 with one line: `fatal: <what>` at ERROR (skycued), `<program>: <what>` on stderr (the tools) |
| web UI header at 1280 px and wider | Weather and Table wrapped to a second row | one row; 12 px text at 1401-1700 px, 11 px at 1400 px and narrower; a plain stat too wide is cut with an ellipsis and shows its full text as a hover title |

## Sanitizers

| what ran | how long | findings | fixes | accepted |
|---|---|---|---|---|
| ASan + UBSan canary (`run-sanitizers.sh asan --canary`) | seconds | the planted UBSan signed overflow and ASan heap overflow both reached `sanitizer-logs/` | the reports of the two runtimes share one `log_path` (ASan's land in `ubsan.<pid>`): the canary matches by report text, not file name ("sanitizers: canary proves ASan and UBSan logs, guarded --clean, cached log dir") | none |
| ASan + UBSan, whole ctest suite (47 tests at the time), twice; `-R '^inputs$' --repeat until-fail:5` | about a minute of test time per run, slow factor 4 | 0 | none needed | none |
| TSan canary | seconds | the planted race reported | the TSan runtime also reads `UBSAN_OPTIONS`, whose `log_path` won: each mode now exports only its own variables ("tests: clean ThreadSanitizer run") | none |
| TSan, whole suite, until clean twice | about 54 s of test time per run | 2 races, both in test code | `inputs`: the UDP sender thread called `fixture()`, whose `BOOST_REQUIRE_MESSAGE` writes Boost.Test state the main thread writes too; the fixture is read before the thread starts (`test_inputs.cpp` `edit_while_receiving_rebinds_same_port`, the test itself is the reproducer under TSan). `web_server`: the test wrote on a Beast websocket stream while `WsClient`'s reader thread was reading it; `WsClient::sendText` now writes the frame to the socket (`test_web_server.cpp` `ws_snapshot_then_events`). Both in "tests: clean ThreadSanitizer run" | no suppression (`tsan.supp` and `lsan.supp` not created) |
| Final reruns | 2 min 28 s (asan) and 2 min 1 s (tsan) from a fresh tree, build included; 52 s of test time each | 0 | | |

No daemon or library code raced, leaked or hit undefined behaviour in the
suite. Skipped inside passing tests, for reasons outside the sanitizers:
`ui_playwright` (Playwright not in the tree), the Leaflet part of `web_ui`,
the readsb full-stack part of `rawtools`.

## Fuzzing

Nine libFuzzer harnesses (ASan + UBSan, clang), one row per target. "First
runs" are the runs of the task that added the harness, 600 s each after its
smoke runs; "final" is the rerun on the finished branch, 600 s each on a
fresh corpus from the committed seeds.

| what ran (target) | how long | execs / corpus: first runs; final | findings | fixes (test, commit) | accepted |
|---|---|---|---|---|---|
| `readsb_line` | 5 s smoke and 600 s runs until clean; final 600 s | 3,744,260 / 1871; 4,213,838 / 857 | 1 | `"now"` up to 1e11 s overflowed the int64 nanosecond conversion; the bound is 9e9 s, NaN-proof (`test_readsb_json.cpp` `fuzz_readsb_line_now_overflow`; "readsb json: reject a "now" past the nanosecond clock's range", "fuzz: findings README, NaN-proof readsb "now" bound") | none |
| `raw_formats` | 5 s smoke and 600 s runs until clean; final 600 s | 33,748,006 / 8452; 36,136,347 / 400 | 2 | `parseCsvMessage` multiplied into an int64 before its digit limit, and 10 digits overflowed `secs * 1e9` (`test_raw_formats.cpp` `fuzz_raw_formats_csv_seconds_overflow`; "raw formats: reject CSV seconds past the int64 nanosecond range"). `avrLine` wrapped `epoch_ns * 3` past 2165 (`test_raw_formats.cpp` `fuzz_raw_formats_avr_ticks_wrap`; "raw formats: compute AVR ticks without wrapping") | none |
| `recording` | 5 s smoke, 10 s and 600 s runs until clean; final 600 s, and 600 s again after the fix | 4,345,898 / 1469; 2,334,032 / 852, 1 finding; after the fix 3,610,645 / 2461 | 2, 2 more in review, 1 in the final rerun | the replay pacing delay was clamped above only (`test_recorder.cpp` `fuzz_recording_replay_delay_overflow`; "replay: clamp the pacing delay at both ends"). `parseIso8601Ms` overflowed outside 1677-2262 (`test_recorder.cpp` `fuzz_recording_iso8601_out_of_range`, `test_capture.cpp` `fuzz_recording_capture_time_out_of_range`; "time: reject ISO 8601 times outside the nanosecond clock's range"). Review of the harness: replay offsets, gaps and loop shifts were plain int64 subtractions (`test_recorder.cpp` `replay_offset_saturates_across_the_clock_range`; "replay: saturate time differences across the clock's range"), and (from the sweep's reviews) the loop shift's zero factor saturated (`test_recorder.cpp` `replay_loop_shift_zero_factor_is_zero`; "replay, web: zero loop shift never saturates; underlay and shown-report fixes"). Final rerun: `parseCaptureLine` accepted an `input` with invalid UTF-8 (an overlong `C0 A3`, which PropertyTree lets through) that `captureLineJson` writes as U+FFFD, so the line did not round-trip (the harness's `back->input == c->input` check); such a line is refused (`test_capture.cpp` `fuzz_recording_capture_input_invalid_utf8`; "capture: refuse a line whose input is not valid UTF-8") | none |
| `fisb_decoder` | 5 s smoke and 600 s runs until clean; final 600 s | 1,718,350 / 1165; 2,615,127 / 1228 | 0 | none needed | none |
| `iqgen_coding` | 5 s smoke and 600 s runs until clean; final 600 s | 25,775,026 / 127; 17,575,067 / 112 | 0 | none needed | none |
| `web` | 5 s smoke and 600 s runs until clean; final 600 s | 5,153,330 / 2102; 5,791,451 / 2174 | 0 (1 in review) | review before the runs: the JSON escapers copied invalid UTF-8 verbatim; one shared escaper writes U+FFFD (`test_readsb_json.cpp` `json_writer_output_is_valid_utf8`, `test_capture.cpp` `capture_line_json_is_valid_utf8`; "json: write invalid UTF-8 as U+FFFD; share the API request builder") | none |
| `ws` | 5 s smoke, 600 s runs (two stopped on the harness fault); final 600 s | 590,648 / 711; 633,210 / 546 | 2 harness faults, 0 in the server | the harness's `connect` failed with EINTR under libFuzzer's timers; it retries ("fuzz: web request, WebSocket inbound and JSON helper harnesses") | none; the two faults were in the harness, fixed there |
| `pingstation` | 5 s smoke and 600 s runs until clean; final 600 s | 2,031,861 / 3840; 2,861,040 / 1324 | 1, and 1 more in review | `parseDeviceTime` overflowed for a year outside 1678-2261 (`test_pingstation.cpp` `fuzz_pingstation_device_time_out_of_range`; "pingstation: reject device times outside the nanosecond clock's range"). Review: `checkDeviceTime`'s `t - arrival` overflowed for an early device time (`test_pingstation.cpp` `fuzz_pingstation_clock_check_far_apart`; "pingstation: overflow-safe device clock check"). The CAT021 framer's buffer was already bounded, now pinned (`test_cat021.cpp` `framer_buffer_bounded_on_garbage`; "cat021: framer buffered() and a bound test on garbage input") | none |
| `config_files` | 5 s smoke and 600 s runs until clean; final 600 s | 4,499,704 / 8744; 4,502,219 / 2075 | 0 | none needed | none |

In all: 7 parser bugs found by the fuzzer runs (six integer or
float-to-integer overflows in time conversions, and the capture line that
did not round-trip, found by the final rerun), and 4 more found in review:
the replay offset saturation, the loop shift's zero factor, the device
clock check and the invalid UTF-8 in JSON output. All 11 are fixed, each
with a regression test. The canary (`run-fuzz.sh readsb_line --canary`) proved
that a crash becomes a finding. `fuzz_smoke` (5 s per target) is the ctest.

## Static analysis

| what ran | how long | findings | fixes | accepted |
|---|---|---|---|---|
| clang-tidy 21 (`bugprone-*`, `cert-*`, `performance-*`, three readability checks), 82 translation units | a few minutes | 281 | about 55 code fixes: `strftime` length, two `int64_t` widenings in `device_time.cpp`, by-`const&` parameters, `reserve()`, `main()` of `skycued`, `skycue-rawrec` and `skycue-pingsim` catching what escapes, `parseHostPort`'s assignment in a condition, optional conversions, `JsonObject` taking `const ptree&`, a `constexpr`, a merged branch clone ("skycue: clang-tidy performance fixes", "skycue: clang-tidy bugprone fixes", "skycued, skycue-rawrec, skycue-pingsim: report what escapes main()"). Options: asio `error_code` returns unchecked, constant multiplications | 45 inline NOLINTs with reasons ("clang-tidy: suppress the reviewed false positives, each with its reason"); checks off with their reasons: `bugprone-unchecked-optional-access` (66, all false positives), `performance-enum-size` (31, ABI), `performance-inefficient-string-concatenation` (5, error messages) |
| cppcheck 2.9 `--enable=warning,portability` | a minute | 5 | none: all structural false positives | 4 `<id>:<file>` suppressions with reasons ("skycued: main catch-all logs through LOG_ERROR; cppcheck suppressions by file") |
| `-Werror`, whole tree, gcc 11 and clang 21 | one build each | 4 | `Engine::io_` unused (removed), a lambda capturing a structured binding (C++17) | two gcc 11 `-Wstringop-overread` false positives in test code, each a local pragma with a comment ("skycue: build clean with -Werror under gcc 11 and clang") |
| Final rerun, `run-static.sh` | 66 s, `-j 16` | 0 | | |

Result: clang-tidy 281 -> 0, cppcheck 5 -> 0. The runner refuses a build
tree that is not its own ("run-static: refuse to reconfigure a build tree
that is not its own"). gcc 8.5 under `-Werror` is checked by the container
verification.

## Soak

| what ran | how long | findings | fixes | accepted |
|---|---|---|---|---|
| `soak_eval.py --self-test` (ctest `soak_eval`) | seconds | the pass rule on 6 synthetic series | none needed | none |
| `soak_short` (ctest, `SKYCUE_SOAK_TESTS=ON`) | 2 min at 5 s | PASS (A's store filling makes 2 min say nothing about leaks: `--growth 1.0`) | none needed | the short run proves the CSV and the rule, not the absence of a leak |
| A killed mid-run; an unreadable port | under a minute each | `FAIL: skycued A exited (status 137)`, `FAIL: cannot read the web port of A`, everything started was stopped | none needed | none |
| 4 hours, two instances (A: FIS-B replay loop; B: pingStation JSON at 5x), one WebSocket and one cue TCP client each, 60 s samples ([hardening-soak-4h.csv](hardening-soak-4h.csv)) | 240 min, 241 samples per instance | `PASS A: rss 10904 -> 10904 kB (+0.0%), fds 12..12`; `PASS B: rss 7744 -> 7748 kB (+0.1%), fds 13..13`; threads 1 throughout; A 30268 uplinks (193218 products, 0 bad frames), B 824402 observations and 12 tracks; no reconnect, no warning or error logged | none needed | `/api/stats` reports `uat_uplink=0` for replayed uplinks (the counter comes from the dump978 decoder only; `/api/fisb/status` counts them): a counter gap seen during the soak, not a leak, outside this sweep's scope. A run shorter than about 40 minutes fails on A at the default growth 0.05 (documented) |

A's RSS rose from 7936 kB at t = 0 to 10904 kB within its first 10 minutes,
as its FIS-B store filled, and did not move again; B's moved by 40 kB in
four hours. The fd counts were flat in the window (A 11 at t = 0, before its
WebSocket client connected).

## Parked minors

One row per ledger item; "how long" is "one change" (one small commit) or "no change".

| what ran (item) | how long | findings | fixes (test, commit) | accepted |
|---|---|---|---|---|
| `readSmallFile` / `readRegularFile` blocked the io thread on a FIFO | one change | open() on a FIFO with no writer blocks the io thread: the daemon froze | `test_web_server.cpp` `tiles_meta_fifo_is_ignored`, `static_fifo_is_404`; "web: never block on a FIFO in the www dir; meta.json uncached", "web: readRegularFile clears O_NONBLOCK after the checks" | none |
| `tiles/meta.json` cached for a day | one change | every `/tiles/` path had `max-age=86400`, also the file a rebuild replaces | `test_web_server.cpp` `tiles_meta_is_not_cached`; same commit | none |
| input names: C1 controls and invalid UTF-8 | one change | only bytes below 0x20 and 0x7f were refused | `test_input_def.cpp` `name_rejects_c1_and_bad_utf8`, `clip_for_message_keeps_utf8_whole`; "inputs: names must be valid UTF-8 without C1 controls" | documented behaviour change: upgrade note in [operating.md](operating.md) |
| `poller_destroyed_mid_flight` on failure | one change | the future's destructor joined a stuck `io.run()`: the binary hung | `test_status_poller.cpp` `poller_destroyed_mid_flight`; "tests: poller_destroyed_mid_flight fails instead of hanging" | none |
| parallel ctest flakes | one change | `client` waited for the callback, not the drop; `pingsim` (c) demanded every datagram; `main_serves_with_overrides` port race | libskycue `test_client.cpp` `tcp_bad_magic_closes_connection`, `test_pingsim.sh` case (c), `test_web_server.cpp` `main_serves_with_overrides` port retry; "tests: three parallel-run flakes" | none |
| `main()` catch-all | one change | wrote to stderr instead of the log | fixed (no test: a log-call change in `main()`, exit status 1 unchanged); "skycued: main catch-all logs through LOG_ERROR; cppcheck suppressions by file" | none |
| inputs write check order; JSON `null` in the inputs file | one change | the docs did not give the real order or the `null` reading | none (docs) | documented: "docs: inputs write check order; input name rule and upgrade note" |
| CAT021 random test indentation | one change | a mis-indented `break` | fixed (no test: indentation only, `git diff -w` shows one split line); "tests: indent the CAT021 framer random test" | none |
| late `timed_out` in the status poller | no change | a late flag after the reply | none | accepted (harmless) |
| `TrackState::last_input` duplicates `latest.input_id` | no change | duplicate field | none | accepted (removing it is a refactor) |
| inputs-file deletion test | no change | asked for | none | already resolved (`test_inputs.cpp`, the deletion case) |
| `-Wconversion` test lines from the gcc 8.5 container run | no change | warnings in test code | none | already resolved (receivers branch; the `-Werror` pass above) |
| `run-static.sh --build-dir` | one change | reconfigured a tree that was not its own (dropped `SKYCUE_UI_TESTS` from it) | fixed (no test: checked by hand, a foreign tree refused with its cache unchanged); "run-static: refuse to reconfigure a build tree that is not its own" | none |
| Pillow check under `python3 -I` | one change | skipped with a user-site-only Pillow | `tests/superbuild/test_make_tiles.sh`; "tests: make_tiles runs the Pillow check without -I for a user-site Pillow" | documented: `scripts/make-tiles.sh --palette` needs Pillow for the isolated interpreter on the baking machine |
| receivers compare: null altitude | one change | raised a TypeError | `tests/superbuild/test_receivers.sh`; "tests: receivers compare reports a null altitude as a mismatch" | none |
| `do_status` uptime | one change | needed `ps` | `tests/superbuild/test_start_script.sh` step 9c; "skycue-start: status takes the uptime from /proc when ps has none" (step 9 no longer sources the script) | none |
| symlinked `sources/` in the offline bundle | one change | the bundle stored only the link | fixed (no test: checked by hand, `tar -tf` lists the file through the link); "make-offline-bundle: dereference a symlinked sources/" | documented in `building-rhel.md` |
| `make-locations.py` dropped rows | one change | rows skipped for a bad id or no position were not counted | `tests/superbuild/test_make_locations.sh`; "make-locations: count the rows skipped for a bad id or no position" | none |
| `tests/ui/run.sh` whole-test SKIP; Playwright stdin | one change | a SKIP passed; the job read the terminal | ctest `ui_playwright` reports Skipped with an empty `SKYCUE_UI_NODE_MODULES`; "ui tests: a whole-test skip exits 77 and ctest reports it skipped" | none |
| tile underlay below a region-edge tile; `failed` counter | one change | the underlay could be an edge tile too; pruned tiles counted as failed; a missing underlay fell to an edge level | `tests/js/app_pure.test.js` "nearestInsideAncestor skips region-edge ancestors" and Playwright tiles cases in `tests/ui/ui.spec.js` ("web: tile underlays come from the nearest ancestor inside a region"); "underlayNextLevel stays on ancestors inside a region" ("replay, web: zero loop shift never saturates; underlay and shown-report fixes") | none |
| the report shown from the list and the 300-symbol cap | one change | the shown report could be cut; closing the card did not let it go | `tests/js/app_pure.test.js` "capSymbols keeps the shown report" ("web: the report shown from the list stays through the symbol cap"); "closeReportCard forgets the shown report" (same commit as the underlay) | none |
| header buttons at 1280 px | one change | Weather and Table wrapped | `tests/ui/ui.spec.js` `header_fits_one_row_at_1280`, `header_long_stat_is_ellipsised_on_one_row`; "web: the header strip stays on one row from 1280 px", "ui tests: header one-row checks allow the designed stat ellipsis" | documented: the 1401-1700 px step and the stat ellipsis in `style.css` |
| `make-locations.py` mode; the A/V summary | no change | asked for | none | already resolved (mode 100755 in git; the summary in `scripts/make-locations.md`) |
| `tileMissing` growth | no change | the map of missing tiles | none | accepted (entries expire on lookup; bounded by the tiles visited) |
| antimeridian culling of weather symbols | no change | symbols across 180 degrees | none | accepted (FIS-B coverage never crosses 180 degrees) |
| exact UDP counts in `test_receivers.sh` | no change | could fail on a loopback drop | none | accepted (no drop seen; a drop fails with a clear message) |
| `run.sh` Playwright process group; `test_parent_project.sh` exit 77 | no change | asked for | none | already resolved (`tests/ui/run.sh` stops Playwright as a process group, "ui tests: run.sh stops Playwright as a process group"; `tests/test_parent_project.sh` exits 77 and `parent_project` has `SKIP_RETURN_CODE 77` in `tests/CMakeLists.txt`) |
| `soak.sh` on a node without WebSocket; literal `/tmp` work dir; header comment | one change | the run failed late instead of skipping; a `${TMPDIR:-/tmp}` fallback; a stale comment | fixed (no ctest: checked once with a node that fails the probe, exit 77, nothing started); "soak: skip without node's WebSocket, work dir under --out; comment notes" | none |
| `JsonObject` keys with '.' | one change | lookup rule unstated | none (comment) | documented (header comment: looked up literally, no path syntax) |
| `skycue-dump` invalid UTF-8 | one change | a behaviour change from the shared escaper | none (docs) | documented: U+FFFD instead of the bytes ([operating.md](operating.md), skycue-dump) |

## Seen in the final reruns

- `ui_playwright` `header_fits_one_row_at_1280_with_fisb_stats` (the
  2015-sample variant, `tests/ui/ui.spec.js`) failed now and then (2 of 40
  repeats, 3 of 40 before this report's polish commits): a long live value
  (`radar age 14 min 14 s`) at 1280 px got the stat ellipsis, which is the
  layout's design, and the test rejected any cut. Fixed in the test ("ui
  tests: header one-row checks allow the designed stat ellipsis"): the
  header cases check only that every control is on one row, and the new
  case `header_long_stat_is_ellipsised_on_one_row` sets a long stat value
  and checks the ellipsis and the single row. Each header case then passed
  40 of 40 repeats.

## Deliberately left

- `bugprone-unchecked-optional-access`, `performance-enum-size` and
  `performance-inefficient-string-concatenation` are off (66, 31 and 5
  findings): false positives, libskycue's ABI, error-message building. The
  45 NOLINTs and 4 cppcheck suppressions carry their reasons.
- The two gcc 11 `-Wstringop-overread` false positives in test code, behind
  local pragmas.
- `/api/stats` `uat_uplink` stays 0 for replayed uplinks (a counter gap, not
  a leak; out of scope).
- A soak shorter than about 40 minutes fails on instance A at growth 0.05;
  the default stays, `soak_short` uses `--growth 1.0`.
- The late `timed_out` in the status poller, `TrackState::last_input`,
  `tileMissing` growth, antimeridian culling of weather symbols, the exact
  UDP counts in `test_receivers.sh`: accepted, reasons in the table above.
- When wide live numbers do not fit, the header cuts plain stat labels with
  an ellipsis rather than wrapping the buttons.
