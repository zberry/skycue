# Changelog

Each release says what changed here. Changes to the cue protocol are also
recorded in [docs/protocol.md](docs/protocol.md).

## 0.1.0

Unreleased. The first public release.

- Every write route answers 400 to a request body that is not a JSON object.
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
- TCP outputs write queued frames in contiguous blocks of up to 64 KiB, so a busy
  hub drains its send queues far faster and no longer risks cutting a healthy
  consumer under its own load.
