# Changelog

Each release says what changed here. Changes to the cue protocol are also
recorded in [docs/protocol.md](docs/protocol.md).

## 0.1.0

Unreleased. The first public release.

- Inputs of six kinds (readsb JSON, Beast frames, 978 MHz UAT lines, dump978-fa,
  pingStation 3 as JSON or ASTERIX CAT021, and replay of recordings) over TCP
  or UDP, merged into one track table, with several of a kind at once.
- A hub that serves the raw frames and texts on to your own programs, each
  output with its own consumers and counters.
- Decoding of 1090 MHz Mode S and 978 MHz UAT in skycued, including
  geometric altitude, Comm-B registers, target state and ACAS advisories.
- Forwarding decisions by range, elevation angle and allow and block lists
  that can be changed live, and cues in a documented wire protocol over TCP or
  UDP, with the C++ client library `libskycue` and its ECEF helpers.
- A web map that loads nothing from the internet: trails, rings, saved views,
  alerts, statistics, a Feeds tab with the health of every input, and offline
  map tiles.
- FIS-B weather from the 978 MHz uplinks (NEXRAD radar, METAR, TAF, PIREP,
  winds aloft, NOTAMs), drawn on the map.
- Recordings and statistics kept across restarts, and the command-line tools
  `skycue-dump`, `skycue-rawrec`, `skycue-rawplay`, `skycue-pingsim`,
  `skycue-stats`, `skycue-fisb` and `skycue-iqgen`.

## Development history

Dated records moved here from the documents that describe the procedures.

### Compiler sweep with `SKYCUE_WERROR=ON`

From `docs/hardening.md`.

Whole tree (daemon, tools, tests, examples), Release:

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
- gcc 8.5 (Boost 1.66): proved on the sweep's final head by
  a clean-room build with `SKYCUE_WERROR=ON` (0 compiler
  warnings), and gcc 11 again; both plain and with
  `SKYCUE_WERROR=ON`.

### Final review regeneration (2026-11-02)

The golden was regenerated after the final review's store fixes. Against the
previous golden (1749 lines): the `fisb_text`, `fisb_radar` and
`fisb_raster` records are identical, and the `fisb_expired` records name the
same keys at the same times. In `fisb_twgo` (539 -> 566 records, 464 -> 462
reports):

- every record gains `kind: "twgo"` and `pending`;
- the 4 records of SIGMETs 19-7540 and 19-7086 (report year 2019 in 2020
  data) are gone: the store rejects another year's report
  (`store.wrong_year`). 19-7540 was adjudication 5 (FD's `BAD_MESSAGES`);
  19-7086 is now allowed in the reverse check as "FD TWGO report of another
  year than the data: rejected by the store";
- 31 G-AIRMET (14) records are added: reports published `pending: true`
  (start 09:00Z, after their reception) published again `pending: false`
  when the clock reaches their start. Their shapes are those of the same
  report's earlier record. 132 records are pending in all; the previous
  golden drew 134 such records as active;
- `expires` changed for 327 of the 462 reports: NOTAMs, AIRMETs, SIGMETs,
  G-AIRMETs and CWAs now end at the earlier of their end and 70 min after
  their last reception.

`--golden-only` passes on the new golden, and the fisb-decode cross-check
was re-run on it on 2026-11-02 (levels 0-2 regenerated: 963, 963 and 9961
lines, no `.ERR` file). Result `OK`; the table above holds these numbers.
Against the previous run only the shape counts moved: 216 exact and 175
single-altitude (the 31 republished G-AIRMET records bring their shapes
again, 11 exact and 20 single-altitude). The "another year" allowance was
not needed, because no FD TWGO report of 2019 is checked for coverage.
Adjudication 5 is still used, by the two 2020 stuck SIGMETs named there,
not by 19-7540.

### Stuck-SIGMET regeneration (2026-11-02)

The golden was regenerated after the store began rejecting stale-on-arrival
AIRMET, SIGMET and CWA reports. Against
the previous golden (1776 lines) exactly 4 lines are removed and none added
or changed: the text and the text-plus-graphics records of SIGMETs
12-20-7489 and 12-20-7676 (`CONVECTIVE SIGMET 99C`, start 2020-07-06T20:55Z,
graphics end 2021-01-01). `fisb_twgo` goes from 566 to 562 records and 462
to 460 reports; no `fisb_expired` record named them (they were still live at
the end of the capture). `skycue-fisb --summary` counts
`store.stale_start 6`: the receptions of their halves.

`--golden-only` passes (the previous golden now fails its new stale check
on 12-20-7489), and the fisb-decode cross-check was re-run on 2026-11-02
against the same level-2 output (9961 lines). Result `OK`; every matched
count in the table above is unchanged (they never counted the two stuck
SIGMETs), adjudication 5 is now unused (0), and the other allowed counts
are unchanged: 381, 265, 175, 3 and 1.
