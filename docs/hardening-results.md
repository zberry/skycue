# skycue hardening: results

This page is a record of what the fuzz harnesses found and the fixes that followed, not a procedure. The procedure is in [hardening.md](hardening.md#fuzzing).

## Harnesses and the fixes they led to

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
started, a RuntimeConfig on its own `skycue.toml` under
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
