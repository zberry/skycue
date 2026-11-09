# skycued recording format

`skycued` records two streams, each as JSONL and/or CSV (`RecorderConfig::jsonl`,
`RecorderConfig::csv`), and a third, JSONL only:

- **received**: every observation read from readsb (`obs`) and every UAT uplink
  (`uat_uplink`), before any filtering.
- **sent**: every track update the forwarder sent, every drop, and every filter change.
  `skycued` does not record its periodic keyframe bursts (they repeat what was already
  sent); the `keyframe` event exists in the format for other writers.
- **fisb** (JSONL only; with `fisb.enable = true` and `jsonl` in `recording.formats`): the
  FIS-B weather decoded from the uplinks, one record per change of the current picture
  (see [FIS-B stream](#fis-b-stream)).

`skycued --replay FILE` plays back a *received* JSONL file (see [Replay](#replay)).

## Files and rotation

Files are named `<dir>/<stream>-YYYYMMDD-HHMMSS.jsonl` and `.csv`, where the stamp is the
UTC time the file was opened and `<stream>` is `received`, `sent` or `fisb`. The JSONL and CSV
files of one stream are opened, rotated and named together. If a name is already taken
(two rotations in the same second), `-1`, `-2`, ... is appended:
`received-20261101-123456-1.jsonl`. A missing `dir` is created (`mkdir -p`).

Before each record the recorder checks whether either current file has reached
`rotate_bytes` (default 256 MiB) or the files are `rotate_after` old (default 1 h); if
so it closes them and starts a new pair, each CSV with a fresh header line. A value of 0
disables that check.

Writes are buffered; the recorder flushes every 100 records, on rotation and on exit.
Its owner (the Engine) is meant to call `flush()` about once a second as well, so a
crash loses at most about a second of records.

If files cannot be created or written (a full disk, say), the recorder logs one ERROR,
drops records (counted in `droppedRecords()`), and tries to reopen its files 5 s later,
doubling the wait after each failed attempt up to 300 s. The first attempt of such a
failure streak starts a new pair of files; every later attempt in the same streak
appends to that same pair (a line cut short by the failure is ended first), so a disk
that stays full yields at most one extra pair per stream. While recovering it flushes
after every record, and once a flush succeeds it logs one INFO line with the number of
records dropped, and the wait goes back to 5 s for the next failure. skycue-rawrec's
raw files (`beast-*.bin`, `uat-*.txt`) follow the same rules, per file, counting dropped
bytes. A record cut short by the failure is not repaired. In a `.bin` the next frame
starts with its own 0x1a, so skycue-rawplay (and readsb) skip the cut bytes and read
every following frame normally; only when the cut falls between the two bytes of an
escaped 0x1a does the cut frame swallow the start of the next one, coming out as one
malformed frame whose CRC readsb rejects. In a `.txt` the cut line runs into the next
one, which leaves that one merged line unusable.

## Retention (`recording.max_gb`)

With `recording.max_gb` above 0, the recordings under `recording.dir` and `recording.dir/raw`
together are kept within that many gigabytes (10^9 bytes): each time skycued or
skycue-rawrec starts a new file (at startup and on every rotation), it deletes the
oldest files until the total is within the budget. Only files named as above count
and are ever deleted: `received-`, `sent-`, `fisb-`, `beast-` and `uat-`, then
`YYYYMMDD-HHMMSS`, an optional `-N`, and `.jsonl`, `.csv`, `.bin` or `.txt`; and the
pingStation files below, `capture-<id>-` (`.jsonl`) and `raw-<id>-` (`.csv`, `.txt`)
with `<id>` an input id (`[a-z0-9-]`, 1 to 32 characters). Other files (compressed
copies, notes, anything renamed) are neither counted nor touched. "Oldest" is by the
time stamp in the name, then `-N`. Never deleted: the files being written; the newest
file of each stream (the whole prefix, so `raw-ps-north` and `raw-ps-south` count
apart) and extension in each of the two directories, by name and by modification time; and any file modified in the last 10 minutes (the
other process may be writing it, and a clock step or a restart can give a live file an
older name). So the total can stay above the budget by those files; skycued then logs
a WARN ("still N bytes over the cap; only open/recent files remain"). `recording.dir/raw`
is not followed when it is a symlink. Each deletion is logged at INFO. A recording that
is being replayed when retention deletes it stays readable to the replay until it
closes the file (POSIX unlink). `recording.max_gb = 0` (the default) deletes nothing.

## pingStation capture and raw frames

These files are written by skycued's pingStation inputs (see
[operating.md](operating.md#the-inputs-file)) directly under `recording.dir`, with the
rotation (256 MiB / 1 h), write-failure handling and retention above. They are
flushed about once a second and when the input stops.

**Capture** (`"capture": true` on a `pingstation_json` or `pingstation_cat021` input):
`capture-<id>-YYYYMMDD-HHMMSS[-N].jsonl`, one line per UDP datagram or, over TCP, per
CAT021 block the framer cut from the stream (bytes it dropped while resynchronising
are not captured). Every datagram is captured, whether it parses or not; a JSON
datagram is stored the same way, as the hex of its text. Each line is exactly:

```
{"t":"2026-11-02T10:15:00.123Z","input":"ps-north","transport":"udp","hex":"1500..."}
```

`t` is skycued's receive time (UTC, milliseconds), `transport` is `udp`,
`tcp_connect` or `tcp_listen`, and `hex` is the bytes as received, lowercase. Readers
accept the keys in any order and uppercase hex, and refuse a line whose `input` is not
valid UTF-8 (`parseCaptureLine` in `skycued/src/asterix/capture_line.hpp`).

`skycue-dump --capture FILE` prints a capture with its CAT021 records decoded,
and `skycue-pingsim --capture FILE --udp HOST:PORT` (or `--tcp`, `--tcp-listen`)
replays it into an input at the recorded pace. `skycue-pingsim --as json|cat021`
synthesises pingStation traffic from a received-stream recording or a readsb
JSON capture instead. See
[operating.md](operating.md#a-pingstation-without-hardware-skycue-pingsim).

**Raw frames** (`recording.raw = true`, `pingstation_json` inputs): the `detail.rawData`
frames of each datagram, written in skycue-rawrec's line formats so that
`skycue-rawplay` replays them through readsb and
dump978 like any raw capture:

- 1090 frames (7 or 14 bytes) go to `raw-<id>-YYYYMMDD-HHMMSS[-N].csv` as
  `<epoch_seconds>.<9-digit nanoseconds>,<HEX>` (uppercase), for example
  `1793620798.412907153,8DAC5E815821C2A65E8F1B30D47A`. rawplay sends them to
  readsb's raw input.
- UAT frames (18 or 34 bytes) go to `raw-<id>-YYYYMMDD-HHMMSS[-N].txt` as
  `<epoch_ms> -<hex>;`, the dump978 raw downlink form with skycue-rawrec's prefix.
  rawplay serves them on `--uat-listen`.

A frame's time is the device's `timeOfReceptionS`/`NS` when `utcSync` is 1 and that
time passes the clock check, else skycued's receive time. `rawData` of any other
length is not written and is counted in the input's `raw_unknown_len`. With
`recording.raw = true` but no `recording.dir`, skycued logs one WARN at startup and writes
nothing.

```sh
skycue-rawplay --raw-in 127.0.0.1:30001 --uat-listen 30978 \
    /data/rec/raw-ps-north-20261102-101500.csv /data/rec/raw-ps-north-20261102-101500.txt
```

## Raw capture of `beast_raw` and `uat_raw` inputs

An input of type `beast_raw` or `uat_raw` with `"capture": true` (needs `recording.dir`, see
[operating.md](operating.md#raw-inputs-and-endpoint-keys)) writes the messages it received,
after framing and before anything else, in the raw tools' formats, directly under
`recording.dir`, with the rotation (256 MiB / 1 h), write-failure handling and retention above:

- `beast_raw`: `raw-<id>-YYYYMMDD-HHMMSS[-N].bin`, the Beast frames in arrival order, escaped as
  on the wire, exactly the bytes a `beast` output serves (so a capture and a consumer of the same
  run are byte-identical). With `"format": "avr"` each AVR line is first turned into a type 2 or
  3 frame with a zero timestamp and signal byte 0, so the file is Beast either way. skycue-rawplay
  replays it with `--beast-in` or `--1090-listen`.
- `uat_raw`: `raw-<id>-YYYYMMDD-HHMMSS[-N].txt`, one line per UAT message as `<epoch_ms> <line>`
  (the line as dump978 prints it, `-hex;` or `+hex;`, the time skycued's receive time in
  milliseconds), like the pingStation raw UAT files. skycue-rawplay replays it with `--uat-listen`.

A frame or line that cannot be written is counted in the input's `capture_dropped`. Writes are
flushed about once a second and when the input stops. Retention keeps the newest file of each
input and extension, like the other `raw-` files.

## Common conventions

- Times are ISO-8601 UTC with milliseconds, e.g. `2026-11-01T12:34:56.789Z`, rounded to
  the nearest millisecond.
- `hex` is the 24-bit address as 6 lowercase hex digits (`a1b2c3`); a non-ICAO address
  has no `~` prefix here: `addr_type` says what kind it is.
- `addr_type` is one of `icao`, `non_icao`, `tisb_trackfile`, `other`.
- `source` is one of `adsb_1090`, `uat_978`, `tisb`, `adsr`, `mlat`, `other`.
- `drop_reason` is one of `timeout`, `filtered`, `source_lost`, `shutdown`, `stale`.
- Real numbers are written with the fewest digits that read back as exactly the same
  value (`37.887976`, `329.67`), always with a `.` decimal point.
- A field whose value is absent is left out of the JSONL object and empty in CSV. A
  non-finite number (NaN, infinity) is treated as absent.

## JSONL

One JSON object per line. Every object starts with `"time"`, then `"type"` (received
stream) or `"event"` (sent stream), then the fields below in the order listed. Strings
are JSON-escaped. Text fields and `raw` are UTF-8: a byte of the input that is not valid
UTF-8 is written as U+FFFD, so every line is valid UTF-8 (the same holds for the capture
lines above).

### Received stream

`"type":"obs"` - one observation. `"time"` is the observation's receive time (readsb's
`now`). Fields:

| key | JSON type | meaning |
| --- | --- | --- |
| `hex` | string | 24-bit address (required) |
| `addr_type` | string | address type (`icao` when absent) |
| `source` | string | where the report came from (`other` when absent) |
| `pos_time` | string (time) | time of the position: readsb's `now - seen_pos`; present with `lat`/`lon` |
| `lat` | number | latitude, degrees (present with `lon`) |
| `lon` | number | longitude, degrees (present with `lat`) |
| `on_ground` | boolean | on the ground; absent when the input did not say (older recordings always have it) |
| `alt_baro_ft` | integer | barometric altitude, feet |
| `alt_geom_ft` | integer | geometric altitude, feet |
| `gs_kt` | number | ground speed, knots |
| `track_deg` | number | track over ground, degrees |
| `baro_rate_fpm` | number | barometric vertical rate, ft/min |
| `geom_rate_fpm` | number | geometric vertical rate, ft/min |
| `callsign` | string | flight id, trailing spaces trimmed |
| `squawk` | string | Mode A code |
| `registration` | string | tail number |
| `emergency` | string | readsb's `emergency` (JSONL only) |
| `category` | string | emitter category, e.g. `A3` (JSONL only) |
| `nic` | integer | navigation integrity category, 0-255 |
| `nac_p` | integer | navigation accuracy (position), 0-255 |
| `rssi` | number | signal level, dBFS |
| `geom_minus_baro_ft` | integer | signed difference geometric minus barometric altitude, feet, when the input reported both in one message or record, or a 1090 TC 19 velocity message gave it; the difference carries its own time (`geom_minus_baro_age_s`) (JSONL only; absent when unknown; replays as recorded) |
| `alt_baro_age_s` | number | seconds from the barometric altitude's measurement to `time`, one decimal (JSONL only; omitted when absent or 0: measured on arrival). A carried 1090 value is older than the frame that carries it |
| `alt_geom_age_s` | number | the same for `alt_geom_ft` (JSONL only; omitted when absent or 0) |
| `geom_minus_baro_age_s` | number | the same for `geom_minus_baro_ft` (JSONL only; omitted when absent or 0). A replay restores the three times from these ages, so the altitude estimates come out as they did live |
| `heading_mag_deg` | number | magnetic heading, degrees (Comm-B BDS 6,0) (JSONL only) |
| `heading_true_deg` | number | true heading, degrees (TC 19 subtype 3 or 4, Comm-B) (JSONL only) |
| `ias_kt` | integer | indicated airspeed, knots (JSONL only) |
| `tas_kt` | number | true airspeed, knots (JSONL only) |
| `mach` | number | Mach number (JSONL only) |
| `roll_deg` | number | roll angle, degrees, positive right wing down (BDS 5,0) (JSONL only) |
| `track_rate_dps` | number | rate of change of track, degrees per second (BDS 5,0) (JSONL only) |
| `sel_alt_ft` | integer | selected altitude, feet, from the MCP/FCU (BDS 4,0 or TC 29) (JSONL only) |
| `sel_alt_fms_ft` | integer | selected altitude from the FMS, feet (BDS 4,0) (JSONL only) |
| `qnh_mb` | number | barometric pressure setting, millibars (JSONL only) |
| `sel_heading_deg` | number | selected heading, degrees (JSONL only) |
| `nav_modes` | array of strings | active autopilot modes, any of `autopilot`, `vnav`, `althold`, `approach`, `lnav`, `tcas` (JSONL only) |
| `acas_sl` | integer | ACAS sensitivity level reported by the aircraft (DF 0 and DF 16) (JSONL only) |
| `acas_ri` | integer | ACAS reply information (DF 0 and DF 16) (JSONL only) |
| `acas_ra` | object | ACAS resolution advisory: `active`, `ara` (14-bit field), `rac` (4 bits), `rat`, `mte`, `text`, and `threat` (`type` `address` with `address`, or `position` with `alt_ft`, `range_nm`, `bearing_deg`; absent when no threat identity) (JSONL only) |
| `acas_ra_age_s` | number | seconds from the advisory's receipt to `time`, one decimal (omitted when 0), so a replay reproduces its 30 s lifecycle (JSONL only) |
| `raw` | string | the original input line (JSONL only): the readsb JSON line; for a pingStation JSON input the datagram text, on the first observation of a datagram only (a `{"aircraft":[...]}` datagram with several aircraft is recorded once, the other observations have no `raw`); for a CAT021 input `cat021:` and the record's bytes as hex |
| `rebroadcast` | boolean | a rebroadcast copy (TIS-B, ADS-R); present only when true (JSONL only) |
| `input` | string | id of the input that produced the observation; absent when not known |

Example (wrapped here):

```json
{"time":"2026-11-01T15:00:00.052Z","type":"obs","hex":"ac5e81","addr_type":"icao",
 "source":"adsb_1090","pos_time":"2026-11-01T14:59:59.452Z","lat":37.887976,
 "lon":-76.261396,"on_ground":false,"alt_baro_ft":18000,"alt_geom_ft":18300,"gs_kt":379,
 "track_deg":329.67,"baro_rate_fpm":64,"geom_rate_fpm":-32,"callsign":"RPA4521",
 "squawk":"4711","emergency":"none","category":"A3","nic":8,"nac_p":9,"rssi":-15.8,
 "raw":"{\"now\":1793545200.052,\"hex\":\"ac5e81\",...}"}
```

`"type":"uat_uplink"` - one UAT ground uplink. `"time"` is when it was received.

| key | JSON type | meaning |
| --- | --- | --- |
| `uplink_hex` | string | the uplink payload as hex |

### Sent stream

`"event"` is `update` or `keyframe` (a track the forwarder sent; `skycued` writes only
`update`), `drop`, or `filter_change` (one at startup describing the active filters, and
one per live change, `"<origin>: <change>"`, e.g. `ui: max_range_nm=120 min_elevation_deg=2`
or `ui: block += a1b2c3`, written before the drops and updates the change causes).
`"time"` is the wall-clock time the record was written.

| key | JSON type | in events | meaning |
| --- | --- | --- | --- |
| `hex` | string | update, keyframe, drop | 24-bit address |
| `addr_type` | string | update, keyframe, drop | address type |
| `source` | string | update, keyframe | source of the track |
| `pos_time` | string (time) | update, keyframe | time of the position |
| `lat` | number | update, keyframe | latitude, degrees |
| `lon` | number | update, keyframe | longitude, degrees |
| `on_ground` | boolean | update, keyframe | on the ground |
| `alt_is_geom` | boolean | update, keyframe | the elevation filter used geometric altitude |
| `alt_baro_ft` | integer | update, keyframe | barometric altitude, feet |
| `alt_geom_ft` | integer | update, keyframe | geometric altitude, feet |
| `gs_kt` | number | update, keyframe | ground speed, knots |
| `track_deg` | number | update, keyframe | track, degrees |
| `vrate_fpm` | number | update, keyframe | vertical rate, ft/min |
| `callsign` | string | update, keyframe | flight id |
| `squawk` | string | update, keyframe | Mode A code |
| `registration` | string | update, keyframe | tail number |
| `nic` | integer | update, keyframe | navigation integrity category |
| `nac_p` | integer | update, keyframe | navigation accuracy (position) |
| `alt_geom_source` | string | update, keyframe | `reported` or `estimated`; only with `alt_geom_ft` (JSONL only) |
| `alt_geom_uncertainty_ft` | integer | update, keyframe | stated error of an estimated `alt_geom_ft`, feet; only when `estimated` (JSONL only) |
| `heading_mag_deg`, `heading_true_deg` | number | update, keyframe | heading, degrees (JSONL only) |
| `ias_kt`, `tas_kt` | integer | update, keyframe | indicated and true airspeed, knots (JSONL only) |
| `mach`, `roll_deg`, `track_rate_dps` | number | update, keyframe | Mach number, roll angle, rate of change of track (JSONL only) |
| `sel_alt_ft`, `sel_alt_fms_ft` | integer | update, keyframe | selected altitude (MCP/FCU, FMS), feet (JSONL only) |
| `qnh_mb`, `sel_heading_deg` | number | update, keyframe | selected barometric setting and heading (JSONL only) |
| `nav_modes` | array of strings | update, keyframe | engaged autopilot modes, as on the received side (JSONL only) |
| `acas_ra` | object | update, keyframe | ACAS advisory as sent: `active`, `ara`, `rac`, `rat`, `mte` (no `text` or `threat`) (JSONL only) |
| `emergency` | string | update, keyframe | emergency status name as sent (`none`, `general`, ...) (JSONL only) |
| `category` | string | update, keyframe | emitter category as sent, e.g. `A3` (JSONL only) |
| `keyframe` | boolean | update, keyframe | the frame's keyframe flag |
| `drop_reason` | string | drop | why the track was dropped |
| `note` | string | filter_change | description of the change |

### FIS-B stream

Written by `Recorder::writeFisb` from the store's events (`fisb::eventRecordJson`). Each
record is `"time"` (the Engine's data clock when the change happened: during a replay the
recording's clock, not the wall clock) and `"type"`, followed by the item's fields. The
field names are shared with the web API; [web-api.md, FIS-B records](web-api.md#fis-b-records)
lists them all.

| `type` | written when | fields after time/type |
| --- | --- | --- |
| `fisb_text` | a product-413 report is new or replaced (METAR/TAF by a newer issue, winds by the next issue of its period; a new PIREP) | the text record |
| `fisb_twgo` | a TWGO report (NOTAM, AIRMET, SIGMET, SUA, G-AIRMET, CWA) becomes complete or its text or graphics change | the TWGO record, shapes included |
| `fisb_radar` | a NEXRAD mosaic (63 regional, 64 CONUS) is published: 10 s after its last new block, or every 30 s while blocks keep coming | the mosaic with every block it holds |
| `fisb_raster` | likewise for the other rasters (70/71 icing, 84 cloud tops, 90/91 turbulence, 103 lightning; one layer per product and altitude code) | the mosaic |
| `fisb_expired` | an item left the store (`"reason":"expired"`), or a TWGO report was cancelled (`"reason":"cancelled"`; it is kept, flagged, 30 min more) | `key`, `product`, `reason`, `expires` |

Unchanged repeats (FIS-B rebroadcasts every product many times) write nothing. Mosaic blocks
are run-length encoded, `"rle":[[value,count],...]` over the 128 bins in order (4 rows of 32
from the block's NW corner), so a 1000-block CONUS mosaic is typically a few tens of kB.

## CSV

RFC 4180: comma-separated, one header line, a field containing `,`, `"` or a line break
is quoted with `"` and its `"` doubled. Absent values are empty; booleans are `0`/`1`;
other values are written as in JSONL. The columns are the JSONL keys, minus the
JSONL-only ones (`emergency`, `category`, `raw`, `geom_minus_baro_ft`, the three `*_age_s` keys, `alt_geom_source`,
`alt_geom_uncertainty_ft`); the CSV headers are unchanged. The received stream keeps what
the input said; an estimated altitude appears only in the sent stream, where `alt_geom_source`
says so. Older files without the new keys replay as before.

Received stream header (the `uplink_hex` column is set only on `uat_uplink` rows; `input`
is last, so the columns before it keep their positions):

```
time,type,hex,addr_type,source,pos_time,lat,lon,on_ground,alt_baro_ft,alt_geom_ft,gs_kt,track_deg,baro_rate_fpm,geom_rate_fpm,callsign,squawk,registration,nic,nac_p,rssi,uplink_hex,input
```

Sent stream header:

```
time,event,hex,addr_type,source,pos_time,lat,lon,on_ground,alt_is_geom,alt_baro_ft,alt_geom_ft,gs_kt,track_deg,vrate_fpm,callsign,squawk,registration,nic,nac_p,keyframe,drop_reason,note
```

## Replay

`ReplaySource` reads a received-stream JSONL file and rebuilds each `obs` record from the
fields above (`raw` is carried along but never parsed, so hand-edited recordings work).
A line that is instead a readsb JSON-port aircraft object (a numeric `"now"` and no
`"time"`, as in `tests/fixtures/readsb-sample.jsonl`) is parsed as readsb output,
so a plain capture of readsb's JSON port replays as well.
`"time"` becomes the observation's receive time. A record needs a valid `"time"` and
`hex`; any other key may be left out, but a key that is present must have the right type
and range. `uat_uplink` records are played too, interleaved with the observations by
`"time"`, and reach the Engine's FIS-B decoder with that time (so a recording's own clock
drives FIS-B expiry); one without `uplink_hex` is skipped. Blank lines are skipped; other
lines that are not valid records are counted, skipped, and reported in one warning per
file.

Records are paced by their `"time"` values: with speed `S` (1 = real time, the
default), record *i* is emitted at `start + (time_i - time_0) / S` on a steady clock.
Speed 0 emits them as fast as the event loop allows.

With looping on (`skycued --replay-loop`), playback restarts at the first record when the file ends, with
every receive and position time shifted forward by the recording's span plus its median
gap between records (at least 1 ms), so times keep increasing across the wrap.

To analyse recordings (coverage, rates, gaps, drops), see `skycue-stats` in [operating.md](operating.md#analysing-recordings-skycue-stats).
