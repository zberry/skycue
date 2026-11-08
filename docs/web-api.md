# skycued web API

skycued serves the web UI and its API itself: HTTP/1.1 and WebSocket on
`WEB_BIND:WEB_PORT` (default `127.0.0.1:8080`; `--web-bind` and
`--web-port` override them). `WEB_PORT=0` turns the server off (logged at
INFO). A port that cannot be bound is fatal at startup: skycued logs
`cannot start: web: cannot listen on <bind>:<port>: <reason>` and exits
with status 1, as for the cue port.

There is no authentication; see [Security](#security).

## Security

- **No authentication.** Anyone who can reach the port can read the
  tracks and change the filters, the lists and the inputs. The UI is meant for a trusted
  network only.
- `WEB_BIND` defaults to `127.0.0.1`: only programs on the same machine
  can connect. Set it to another address (or `0.0.0.0` for every
  interface) only when every host that can reach the port may change the
  filters; otherwise reach the UI through an SSH tunnel
  (`ssh -L 8080:127.0.0.1:8080 <host>`).
- Two checks keep a web page from another site, open in a browser on
  the trusted network, from changing anything through that browser
  (cross-site request forgery). Every state-changing request (`PUT`,
  `POST`, `DELETE`) must have:
  - (`PUT` and `POST`; a `DELETE` has no body and needs none) a
    `Content-Type` that starts with `application/json` (a parameter such
    as `; charset=utf-8` may follow), else **415**
    `{"error":"content-type must be application/json"}`. A cross-site HTML
    form cannot send that type, and a cross-site script cannot without the
    browser asking first (which skycued does not answer).
  - when it has an `Origin` header (browsers send one), an origin whose
    host and port are those of the `Host` header (a scheme's default port
    may be left out of either; case is ignored), else **403**
    `{"error":"cross-origin request rejected"}`. `Origin: null` is
    rejected. A request without `Origin` (curl, scripts, the skycue tools)
    is allowed.

  `GET` requests are never refused for these. The page itself sends
  `Content-Type: application/json` on every `PUT` and `POST`.

## Conventions

- Bodies are JSON (`Content-Type: application/json`) unless noted. Numbers
  and booleans are never quoted. A value that is not known is left out
  rather than sent as `null` (a track without an altitude has no
  `alt_baro_ft`; a range or elevation that cannot be computed is omitted).
- Times are ISO-8601 UTC with milliseconds: `"2026-11-01T12:34:56.789Z"`.
- Every JSON reply and every WebSocket item is valid UTF-8: a string from
  input that is not (a callsign or input name with stray bytes, say) has
  each invalid byte written as U+FFFD.
- API responses carry `Cache-Control: no-store`.
- Errors are `{"error": "<what went wrong>"}` with status:
  - `400`: the body is not a JSON object, has an unknown key, a value of
    the wrong type, a value out of range (the same checks as
    `skycue.conf`), or a bad list entry (the message names it). Nothing
    changes.
  - `403`: a `PUT`/`POST`/`DELETE` from another origin
    ([Security](#security)), or a change a built-in input does not take
    ([Inputs](#inputs)).
  - `404`: no such API path (JSON), input or file (plain text).
  - `405`: wrong method; the `Allow` header lists the right ones.
  - `413`: a request body over 64 KiB (the connection is then closed).
  - `409`: an input id that is taken, or any input change while skycued
    replays a recording ([Inputs](#inputs)).
  - `415`: a `PUT`/`POST` without `Content-Type: application/json`
    ([Security](#security)).
  - `429`: `POST /api/stats/reset` within 5 s of the last reset
    ([Statistics](#post-apistatsreset)).
  - `500`: a file could not be written (`skycue.runtime.json`,
    `skycue.inputs.json`, or the baseline files on save; nothing changes),
    or a static file is over 8 MiB or unreadable (plain text).
- Success is `200` with a body, except `POST /api/inputs` (`201`, the new
  input) and `DELETE /api/inputs/{id}` (`204`, no body).
- Connections are kept alive when the client asks (HTTP/1.1 default) and
  closed after 30 s idle.

## Routes

| Method | Path | Request body | Response |
|--------|------|--------------|----------|
| GET | `/api/state` | | the state object (below) |
| GET | `/api/stats` | | the stats object |
| GET | `/api/filters` | | the filters object |
| PUT | `/api/filters` | `{"max_range_nm"?: number or null, "min_elevation_deg"?: number or null}` | the filters object, after the change |
| POST | `/api/filters/save-baseline` | `{}` (ignored, but the JSON `Content-Type` is required) | `{"ok": true, "unsaved": "<text>"}` |
| GET | `/api/lists` | | the lists object |
| POST | `/api/lists/allow`, `/api/lists/block` | `{"add"?: [entry...], "remove"?: [entry...]}` | the lists object, after the change |
| GET | `/api/inputs` | | `{"inputs": [input...]}` ([Inputs](#inputs)) |
| POST | `/api/inputs` | a definition, `id` included | `201`, the input |
| PUT | `/api/inputs/{id}` | a definition (`id` optional, must be `{id}`); a built-in: `{"enabled": bool}` only | the input |
| DELETE | `/api/inputs/{id}` | | `204`, no body |
| POST | `/api/inputs/{id}/enable`, `/api/inputs/{id}/disable` | `{}` | the input |
| GET | `/api/inputs/{id}/status` | | the input's status-poll object |
| GET | `/api/tiles-available` | | `{"tiles": bool, "minZoom"?: int, "maxZoom"?: int, "overlays": bool}` |
| GET | `/api/fisb/status` | | the FIS-B status object ([FIS-B](#fis-b)) |
| GET | `/api/fisb/text?type=&station=` | | `{"records": [...]}`: text records |
| GET | `/api/fisb/shapes?product=` | | a GeoJSON FeatureCollection of TWGO shapes |
| GET | `/api/fisb/radar?product=regional\|conus` | | a NEXRAD mosaic with palette and legend; 404 when there is none |
| GET | `/api/fisb/raster?product=<id>&altitude=` | | another raster layer with palette and legend; 404 when there is none |
| GET | `/api/fisb/locations` | | `{"<id>":[lat,lon],...}`: the positions known for report location ids |
| GET | `/api/stats/summary?from=&to=` | | the statistics summary ([Statistics](#statistics)) |
| GET | `/api/stats/series?res=minute\|hour&from=&to=` | | the time series as columns |
| GET | `/api/stats/coverage` | | the coverage table and the position grid |
| GET | `/api/stats/rssi` | | the signal-versus-range matrix |
| GET | `/api/stats/aircraft?sort=&dir=&limit=&offset=&q=` | | a page of the aircraft ledger |
| GET | `/api/stats/aircraft/{hex}?addr_type=` | | one ledger entry; 404 when unknown |
| POST | `/api/stats/reset` | `{}` | `{"ok": true}`; empties the statistics |
| GET | `/api/filters/history` | | `{"changes": [change...]}`: the last 100 filter changes, oldest first |
| GET | `/api/cues/clients` | | `{"tcp": [client...], "udp": [target...]}`: the cue stream's TCP connections and UDP targets now |
| GET | `/api/config` | | `{"daemon": {...}, "keys": [key...]}`: the configuration running and where each value came from |
| GET | `/ws` | (WebSocket upgrade) | see [WebSocket](#websocket-ws); without an upgrade, 400 |
| GET, HEAD | `/`, `/<file>` | | a static file (see [Static files](#static-files)) |

`PUT /api/filters` and `POST /api/lists/*` change the runtime overlay
(see [below](#runtime-overlay-skycueruntimejson)) with origin `ui`: the
overlay file is rewritten, the Engine applies the change at once (one
`filter_change` record `ui: ...` in the sent stream, then every live track
re-evaluated: drops and sends follow the record), and the response is
sent after all of that. A key left out of the body changes nothing; a
key set to `null` removes that value from the overlay, so the baseline
(`skycue.conf`) value applies again (clearing a value the overlay does
not have changes nothing). Because of how skycued parses JSON, the string
`"null"` is read as `null` too.
`max_range_nm` must be > 0; `min_elevation_deg` in -90..90. List entries
use the list file grammar; adding an entry already listed or removing one
that is not listed is not an error.

`POST /api/filters/save-baseline` is "save as baseline" (below). `unsaved`
is `""` when everything was saved, else what could not be, one problem
per line (those changes stay in the overlay).

`GET /api/filters/history` lists the last 100 changes of the range and
elevation filters, oldest first, kept in memory (lost on restart):

```json
{"changes": [
  {"time": "2026-11-02T14:03:07.120Z", "origin": "startup",
   "filters": {"max_range_nm": 150, "min_elevation_deg": 0}},
  {"time": "2026-11-02T14:10:41.508Z", "origin": "ui",
   "filters": {"max_range_nm": 20, "min_elevation_deg": 0}}
]}
```

Each entry is the new state only, never a difference: the first entry is
the startup state (with the overlay applied), and a page shows what
changed by comparing consecutive entries. `time` is the wall clock, also
during a replay (an operator audit log: when the change was made, not the
data clock's `now`); `origin` is as in the `filter_change` records
(`startup`, `ui`, `file:<name>`, `save-baseline`, ...). The history is
kept whether or not the sent stream is recorded. List changes are not in
it. Other methods: 405 with `Allow: GET`.

`GET /api/cues/clients` lists where the skycue cue stream goes right now:

```json
{"tcp": [
  {"endpoint": "127.0.0.1:41822", "direction": "accepted",
   "connected_since": "2026-11-02T14:03:09.004Z", "bytes_sent": 48213},
  {"endpoint": "10.0.0.7:30600", "direction": "dialled",
   "connected_since": "2026-11-02T14:03:07.311Z", "bytes_sent": 47950}
],
 "udp": [{"endpoint": "10.0.0.9:30501", "datagrams_sent": 1270}]}
```

`tcp` has one entry per open connection: `direction` is `accepted` for a client that connected to `CUE_TCP_PORT`
and `dialled` for one of `CUE_TCP_CONNECT`; `endpoint` is the peer
(`addr:port`, an IPv6 address in brackets, or the configured `host:port` when dialled);
`connected_since` is the wall-clock time the connection was made; and
`bytes_sent` counts the bytes actually written to it (not those still
queued). A closed connection leaves the list. `udp` has every UDP target
that resolved at startup, in configuration order, with the datagrams sent
to it without error. The counts start at 0 for each connection and when
skycued starts. The fields above are the whole object: a later release adds an
optional `output` field (the id of the cue output the entry belongs to) once
outputs can be defined at runtime; clients should ignore fields they do not
know. Other methods: 405 with `Allow: GET`.

`GET /api/config` shows the configuration skycued runs with and facts
about the daemon. It is read only: nothing in it can be changed here.

```json
{"daemon": {"version": "skycued 0.1.0", "started": "2026-11-02T14:03:06.912Z", "uptime_s": 3725,
            "config_path": "/etc/skycue/skycue.conf", "inputs_path": "/etc/skycue/skycue.inputs.json",
            "overlay_path": "/etc/skycue/skycue.runtime.json", "overlay_active": true,
            "overrides_path": "",
            "www_dir": "/usr/local/share/skycue/www", "map_dir": "/usr/local/share/skycue/www",
            "record_dir": "/var/lib/skycue/rec",
            "recording_files": ["received-20261102-140306.jsonl", "sent-20261102-140306.jsonl"],
            "log_level": "info", "mode": "live", "readsb": true, "dump978": true,
            "locations": {"learned_path": "/etc/skycue/fisb-locations.json", "learned": 42,
                          "operator_path": "/etc/skycue/locations.csv", "operator": 61234,
                          "operator_rejected": 0, "operator_overflow": 0}},
 "keys": [
  {"key": "RECEIVER_LAT", "value": "38.9", "source": "conf", "restart": true},
  {"key": "MAX_RANGE_NM", "value": "20", "source": "overlay", "restart": false},
  {"key": "MIN_INTERVAL_S", "value": "1", "source": "default", "restart": true},
  {"key": "WEB_PORT", "value": "8081", "source": "cli", "restart": true}
 ]}
```

`daemon`: `started` is the wall-clock time skycued started and `uptime_s`
the whole seconds since; `config_path` is the `--config` file as given;
`inputs_path` the inputs file in use (resolved as for `INPUTS_FILE`, or
`--inputs-file`); `overlay_path` the runtime overlay file, and
`overlay_active` whether the overlay holds changes not saved to the
baseline (as in the filters object); `overrides_path` the
`skycue.overrides.conf` in `STATE_DIR` that save as baseline writes (see
[Save as baseline](#save-as-baseline); `""` when `STATE_DIR` is unset);
`www_dir` the web UI's files; `map_dir` the directory `/tiles/`,
`/overlays/` and `/api/tiles-available` are read from: `MAP_DIR`, or
`www_dir` when `MAP_DIR` is unset;
`record_dir` is `RECORD_DIR` (`""`:
not recording) and `recording_files` the base names of the recording files
being written now; `log_level` is `--log-level`. `mode` is `"live"` or
`"replay"`; a replay adds `"replay": {"file": "<--replay FILE>", "speed":
<--replay-speed, 0 = unpaced>, "loop": <--replay-loop>}`. `readsb` and
`dump978` say whether the built-in readsb and dump978 inputs are used
(`false` while replaying, and for `--no-readsb` / `--no-uat`), so their
keys' values may not be in use. `locations` is the FIS-B report-location
index now: `learned_path` the `fisb-locations.json` it saves to (`""` when
it is not saved this run, see [operating.md](operating.md#report-locations-fisb-locationsjson)),
`learned` its ids, `operator_path` the resolved `LOCATIONS_FILE` (`""` for
none), `operator` the ids read from it, `operator_rejected` its lines that
did not parse and `operator_overflow` the ids past the 250,000 cap.

`keys` has every `skycue.conf` key, in the order of
`packaging/skycue.conf.example`. `value` is the value running, spelled as
`skycue.conf` would (`yes`/`no`, `host:port host:port`, `""` for an unset
string). `source` is where it came from, the first that applies:
`overlay` (`MAX_RANGE_NM` or `MIN_ELEVATION_DEG` set by the runtime
overlay), `cli` (a command-line option: `--cue-tcp-port`, `--web-port`,
`--web-bind`, `--inputs-file`, `--readsb-host`, `--dump978-host`, or
`--no-uat` for `DUMP978_ENABLE`; `value` is the option's), `overrides` (set
in `skycue.overrides.conf`, `STATE_DIR` only), `conf` (set in
`skycue.conf`), `default`. `restart` is true for every key that takes
effect only when skycued restarts: all but `MAX_RANGE_NM`,
`MIN_ELEVATION_DEG`, `ALLOW_LIST_FILE` and `BLOCK_LIST_FILE`, which apply
at once. Other methods: 405 with `Allow: GET`.

`GET /api/tiles-available` reads the map directory (`MAP_DIR` when it is
set, else the www directory; `www/` below): when `www/tiles/meta.json` exists (written by
the tile preparation script) its `minZoom`/`maxZoom` are passed through;
otherwise the zoom directories `www/tiles/<z>/` that exist give the range;
`{"tiles":false,...}` when there are none. `overlays` (always the last key)
is true when `www/overlays/meta.json` (written by `scripts/maps/make-overlays.sh`)
is readable and a JSON object; the page fetches that file only then.

### Map tiles

Tiles are optional static files under `www/tiles/` (or `MAP_DIR/tiles/`
when `MAP_DIR` is set; `/overlays/` likewise), laid out
`tiles/{z}/{x}/{y}.png` (XYZ scheme, Web Mercator, 256 px), served like any
other file (with `Cache-Control: max-age=86400`). `scripts/maps/make-tiles.sh`
makes them from Natural Earth and writes `tiles/meta.json`:

```json
{"minZoom": 0, "maxZoom": 9, "bbox": [-180, -85, 180, 85],
 "regions": [{"name": "global", "bbox": [-180, -85, 180, 85], "minZoom": 0, "maxZoom": 5},
             {"name": "northeast", "bbox": [-76, 38.5, -66, 46], "minZoom": 0, "maxZoom": 9}],
 "source": "Natural Earth 1:10m", "generated": "2026-11-02T12:00:00Z"}
```

Only the top-level `minZoom` and `maxZoom` (the overall range of all
regions) are read (by `/api/tiles-available`); the other keys are for
people. The page asks `/api/tiles-available` once when it loads: with tiles
it adds a tile layer (zooming out stops at `minZoom`, tiles are asked for up
to `maxZoom` and scaled up past it); without, it draws a graticule. A
missing tile is a 404; the page then tries the tile's ancestors (one zoom
level lower each time, at most 4 levels, not below `minZoom`) and shows the
first one that exists, enlarged and cropped to the square, so a zoom that
only some regions have still shows the coarser tiles elsewhere. The page
also reads `tiles/meta.json` itself for `regions`: a tile that exists but
is not wholly inside the bbox of a region with its zoom is on a region's
edge, and its nearest ancestor is drawn underneath it so that its
transparent part is not blank. URLs that 404 are remembered for 60 s. See
`docs/make-tiles.md`.

### The state object

```json
{
  "now": "2026-11-01T12:34:56.789Z",
  "receiver": {"lat": 38.9, "lon": -77, "alt_m": 100, "antenna_m": 5},
  "filters": {"max_range_nm": 150, "min_elevation_deg": 0, "overlay_active": false},
  "lists": {"allow": ["N123AB"], "block": [], "overlay_active": false},
  "trail_seconds": 120,
  "stats": { ... the stats object ... },
  "tracks": [ ... track objects ... ],
  "fisb": { ... the FIS-B status object ... },
  "inputs": [ ... input objects ... ]
}
```

- `now`: skycued's data clock (the newest observation's receive time plus
  the time since; the wall clock before any). Compare `first_seen` and
  `last_seen` with it, not with the browser's clock.
- `receiver.alt_m` is `RECEIVER_ALT_M`, `antenna_m` is `ANTENNA_HEIGHT_M`.
- The filters object: the effective values, and `overlay_active`: the
  runtime overlay is not empty (the running configuration differs from
  the baseline files; "save as baseline" would change them).
- The lists object: the effective allow and block lists (entries as
  written), and `overlay_active` as above.
- `trail_seconds` is `TRAIL_SECONDS`.
- `fisb`: the [FIS-B status object](#get-apifisbstatus), the same as
  `GET /api/fisb/status` (counts and mosaic heads, never a mosaic's
  blocks). With `FISB_ENABLE=no` it has `"enabled": false` and zeros.
- `inputs`: every input with its state, as `GET /api/inputs` lists them
  ([Inputs](#inputs)).

Keys beyond the basic track/filter data that the UI relies on:
`filters.overlay_active` and `lists.overlay_active` (show "unsaved
changes" / enable "save as baseline"), `trail_seconds` (how much trail to
keep), and in each track `position_time` (when the position was measured)
and `alt_is_geom` (the elevation used the geometric altitude).

The stats object:

| Key | Meaning |
|-----|---------|
| `observations` | observations received |
| `sent_updates`, `sent_drops` | messages forwarded |
| `rebroadcast_suppressed` | rebroadcast (TIS-B, ADS-R) positions ignored because the track had a direct position younger than 10 s |
| `drops_timeout`, `drops_filtered`, `drops_source_lost` | `sent_drops` by reason (shutdown drops are only in `sent_drops`) |
| `drops_blocked`, `drops_out_of_range`, `drops_below_elevation`, `drops_no_position` | `drops_filtered` by the filter decision that dropped the track; they sum to `drops_filtered` |
| `tracks_total`, `tracks_forwarded` | tracks in the table / being forwarded now |
| `uat_downlink`, `uat_uplink` | UAT frames from dump978 |
| `readsb_connected`, `dump978_connected` | booleans: the built-in `readsb` (or a replay) and `dump978` inputs are connected. **Deprecated**: derived from `inputs`, kept for one release |
| `tcp_clients`, `udp_targets` | forwarder destinations now |
| `recorder_dropped_received`, `recorder_dropped_sent` | records the recorders had to drop |
| `web_clients` | WebSocket clients connected now |
| `inputs` | `[{"id", "connected", "messages", "observations", "uplinks", "rejected"}]`: every input's live counters (the full state is in [Inputs](#inputs)), in the order of `GET /api/inputs` |

A track object:

| Key | Meaning |
|-----|---------|
| `hex` | the 24-bit address, 6 lower-case hex digits |
| `addr_type` | `icao`, `non_icao`, `tisb_trackfile`, `other` |
| `source` | `adsb_1090`, `uat_978`, `tisb`, `adsr`, `mlat`, `other` |
| `lat`, `lon`, `position_time` | only when the track has a position |
| `alt_baro_ft`, `alt_geom_ft`, `gs_kt`, `track_deg`, `vr_fpm` | when known |
| `callsign`, `squawk`, `registration`, `nic`, `nac_p` | when known |
| `on_ground`, `alt_is_geom` | booleans |
| `range_nm`, `elevation_deg` | from the receiver; left out when they cannot be computed |
| `decision` | `send`, `allow_listed`, `blocked`, `out_of_range`, `below_elevation`, `no_position`: what the filters decide now |
| `forwarded` | the consumers have this track now |
| `first_seen`, `last_seen` | receive times |
| `emergency`, `category` | as readsb reports them, when known |
| `rssi` | dBFS, when reported |
| `input` | id of the input the newest observation came from, when known |

## Inputs

skycued's inputs are the built-ins (`readsb` and `dump978`, the decoders
skycued connects to; `replay` under `--replay`) and the inputs added
through these routes or the inputs file (see
[operating.md](operating.md#the-inputs-file)), such as a pingStation 3.
Every change is written to `skycue.inputs.json` before it is applied; when
that write fails the answer is `500` and nothing changes.

An input object is the definition, `builtin`, and `state`:

```json
{"id":"ps-north","name":"North mast","type":"pingstation_json","enabled":true,"bind":"0.0.0.0","port":4000,
 "peer":null,"status_url":"http://192.0.2.10/api/v1/status","status_interval_s":30,"time_mode":"utc",
 "capture":false,"builtin":false,
 "state":{"connected":true,"receiving":true,"peers":1,"last_data_age_s":0.4,"clock_offset_s":0.012,
          "status_fail_streak":0,"last_error":"","connects":1,"bytes":181422,"messages":512,
          "observations":498,"uplinks":0,"rejected":2,"records_skipped":0,"sac_sic_mismatch":0,
          "nogo_records":0,"clock_rejects":0,"rejected_peer":0,"raw_unknown_len":0,"status_ok":14,
          "status_fail":0,"capture_dropped":0,
          "device_status":{"lat":38.9,"lon":-77.0,"alt_m":45,"alt_type":1,"gps_status":3,
                           "receiver_status":0,"version":"1.4.2","guid":"0a1b2c3d","device_time":"2026-11-02T12:00:00.000Z",
                           "received_at":"2026-11-02T12:00:00.020Z"}}}
```

The definition carries only the keys that apply to its type and
transport, defaults included (as the inputs file stores it):

| Key | Types | Meaning |
|-----|-------|---------|
| `id` | all | 1 to 32 characters of `a-z`, `0-9` and `-`; `readsb`, `dump978` and `replay` are reserved |
| `name` | all | free text for the UI (`""` by default): valid UTF-8, at most 64 characters, no C0 or C1 control character or DEL |
| `type` | all | `readsb`, `dump978`, `pingstation_json`, `pingstation_cat021` (`replay` is never accepted) |
| `enabled` | all | `true` by default |
| `transport` | `pingstation_cat021` | required, no default: `udp`, `tcp_listen` or `tcp_connect`; left out it is a 400 "transport is required for pingstation_cat021 (udp, tcp_connect or tcp_listen)" (`readsb`/`dump978` connect over TCP, `pingstation_json` is UDP, and neither takes the key) |
| `host` | connecting inputs | the address to connect to |
| `bind` | listening inputs | the local address, `0.0.0.0` by default |
| `port` | all | 1 to 65535; two enabled inputs may not listen on the same port and protocol |
| `peer` | listening inputs | only this source address is accepted; `null` for any |
| `edition`, `sac`, `sic` | `pingstation_cat021` | the CAT021 edition (`"2.5"`), and the SAC/SIC to expect (counted, never used to drop; `null` for none) |
| `status_url`, `status_interval_s` | pingStation types | the device's `http://` status endpoint (`null` for none) and how often it is polled. Use the device's IP address, not a hostname: a lookup that hangs cannot be cut short, and while one is outstanding each poll is skipped and counted in `status_fail` (`last_error` "resolve still pending") |
| `time_mode` | pingStation types | `utc` or `gps`: the clock of the device's timestamps |
| `capture` | pingStation types | write every datagram or block received to `RECORD_DIR`; without `RECORD_DIR` a write is a 400 "capture needs RECORD_DIR" (in the inputs file the entry is kept, with a WARN, and nothing is captured) |

`builtin` (`true` for a built-in) and `state` are output only: a body
with either is a `400`. `state`:

| Key | Meaning |
|-----|---------|
| `connected` | the transport is up: the TCP connection of a connecting input, at least one client of a `tcp_listen` input, the bound socket of a UDP input (connected even when nothing arrives) |
| `receiving` | data arrived in the last 30 s |
| `peers` | TCP connections now: 1 for a connected `tcp_connect` input, the clients of a `tcp_listen` input, 0 for UDP |
| `last_data_age_s` | seconds since data last arrived; absent before any |
| `clock_offset_s` | the newest device timestamp (after the `gps` correction) minus its arrival time; absent when unknown |
| `status_fail_streak` | status polls failed in a row |
| `last_error` | the latest transport, parse or status error, `""` when none |
| `connects`, `bytes`, `messages`, `observations`, `uplinks`, `rejected` | connections made, bytes received, messages decoded (a readsb or dump978 line, a JSON datagram, a CAT021 record), track observations and FIS-B uplinks passed on, data that could not be used |
| `records_skipped`, `sac_sic_mismatch`, `nogo_records` | CAT021 records skipped (simulated, test or RAB records, records without an address, blocks cut short by a length error), with another SAC/SIC than configured, with the NOGO bit set |
| `clock_rejects` | device timestamps more than 10 s from arrival, replaced by the arrival time |
| `rejected_peer` | datagrams or connections from an address other than `peer`; each refused connection, and a refused datagram at most once a second, also sets `last_error` ("datagram from X refused (not the configured peer)"); the Receiver tab shows it beside `rejected` when not 0 |
| `raw_unknown_len` | `rawData` of no known frame length (not written to the raw files) |
| `status_ok`, `status_fail` | status polls answered with a status document, and failed |
| `capture_dropped` | capture lines and raw frames dropped because the file could not be written |
| `device_status` | the device's last status (from its JSON datagrams or the status poll), when there is one: `lat`, `lon`, `alt_m`, `alt_type`, `gps_status`, `receiver_status`, `version`, `guid`, `device_time` (each when reported) and `received_at` |

Counters are the input's lifetime counts: a `PUT` that restarts it keeps
them, a `DELETE` forgets them.

Routes:

- `GET /api/inputs`: `{"inputs": [...]}`, the built-ins first, then the
  added inputs in file order.
- `POST /api/inputs`: creates an input from a definition, which must
  name its `id`. `201` with the input. `400` for a bad definition (an
  unknown key, a key that does not apply to the type, a value out of range,
  a port another input listens on: the message names the key or the other
  input) and `409` for an id that is taken.
- `PUT /api/inputs/{id}`: replaces the definition (the input restarts; the
  same definition changes nothing). An `id` in the body must be `{id}`
  (else `400`); without one `{id}` applies. A built-in takes only
  `{"enabled": bool}`: any other key is `403`. `404` for an unknown id.
- `DELETE /api/inputs/{id}`: stops and removes the input; `204` without a
  body. `403` for a built-in, `404` for an unknown id. Like every write it
  is refused from another origin, but needs no `Content-Type`.
- `POST /api/inputs/{id}/enable`, `/disable`: the body is `{}` (a key is
  `400`); for any input, built-ins included. The input.
- `GET /api/inputs/{id}/status`: what the status poll knows, without the
  rest: `{"id", "status_url" ("" when none), "status_interval_s",
  "status_ok", "status_fail", "status_fail_streak", "device_status"?}`.

A request under `/api/inputs` is checked in this order:

1. The path: any other path under `/api/inputs/` is `404`.
2. The id: an `{id}` that breaks the id rule is `400` `invalid input id ...`
   before anything is looked up (the segment is not echoed).
3. The method: any other method on these paths is `405` with the `Allow`
   header.
4. For a write, the checks every write gets ([Security](#security)): a
   `Content-Type` other than `application/json` is `415` (`DELETE` needs
   none), then an `Origin` of another origin is `403`. A request without an
   `Origin` header (curl, scripts) is allowed, as on the other routes.
5. For a write while skycued replays a recording (`--replay`): `409`, before
   the body is read or the id looked up.
6. The body and the id, as each route above describes.

Ids, keys and paths quoted in an error message are cut to 64 bytes followed
by `...`.

## WebSocket: `/ws`

The UI's live feed. Every message is one JSON text frame.

1. Right after the handshake: `{"type": "snapshot", ...}` with every key
   of the state object.
2. Then the events, as they happen. Every event of one turn of skycued's
   event loop goes out as one message, in order:
   ```json
   {"type": "events", "items": [
     {"type": "observed", ...track object...},
     {"type": "update", ...track object...},
     {"type": "drop", "hex": "a1b2c3", "addr_type": "icao", "reason": "filtered",
      "decision": "out_of_range", "callsign": "UAL1423", "time": "2020-10-30T09:00:17.000Z"},
     {"type": "expired", "hex": "c0ffee", "addr_type": "icao"},
     {"type": "stats", ...stats object...},
     {"type": "filters", ...filters object...},
     {"type": "lists", ...lists object...},
     {"type": "inputs", "inputs": [...input objects...]},
     {"type": "fisb_text", ...}, {"type": "fisb_shape", ...}, {"type": "fisb_radar", ...},
     {"type": "fisb_raster", ...}, {"type": "fisb_expired", ...}, {"type": "fisb_location", ...}
   ]}
   ```
   - `observed`: an observation with a position arrived, for any track,
     forwarded or not (blocked, out of range, below the elevation mask,
     allow-listed...), with its decision, range and elevation now. **Use
     these for the positions on the map.** One per positioned observation,
     so at readsb's position rate (typically about one per second per
     aircraft, more for nearby ones). Observations without a position give
     none.
   - `update`: a track was sent to the consumers (`forwarded` true): the
     "sent" indication, not a position feed (sends are rate-limited and
     happen only for tracks that pass the filters). The `observed` item of
     the same observation follows it.
   - `drop`: a forwarded track was dropped; `reason` is `timeout`,
     `filtered` or `source_lost` (shutdown drops are not reported).
     `decision` (only for `filtered`) is the filter decision that dropped
     it: `blocked`, `out_of_range`, `below_elevation` or `no_position`.
     `callsign` is the track's last known callsign (absent if it never
     had one); `time` is skycued's data clock at the drop. The
     track itself may still be in the table (shown, not forwarded): its
     next state arrives with the next snapshot or update.
   - `expired`: a track that was not being forwarded (so no `drop` is
     sent for it: blocked, out of range, no position...) has not been heard
     for `TRACK_TIMEOUT_S` and left skycued's table. Remove it from the map.
   - `stats`: once a second, so a client can tell a dead connection
     (skycued stopped or the network gone without a close) by their
     absence: the page treats 5 s without one as disconnected, closes the
     socket and reconnects.
   - `filters`, `lists`: the filters or lists changed (from the UI, a hand
     edit of a file, or save as baseline); they come before the drops and
     updates the change caused, in the same message. Both also come
     whenever the runtime overlay changed without changing an effective
     value (save as baseline, or an overlay value set to or cleared back
     to the baseline's), so every page shows the current `overlay_active`.
   - `inputs`: an input was added, changed, removed, enabled or
     disabled, or connected or lost: the whole list, as `GET /api/inputs`
     answers it. At most one per message, however many changes the turn
     had (it is built when the message is sent). A device status (from a
     pingStation datagram or its status poll) is not a change: read it from
     `GET /api/inputs/{id}/status` or the next list. The live counters come
     with every `stats` item instead.
   - `fisb_text`, `fisb_shape`, `fisb_radar`, `fisb_raster`,
     `fisb_expired`, `fisb_location`: FIS-B weather and NOTAMs (see
     [FIS-B WebSocket items](#fis-b-websocket-items)).

   An event that happened before the snapshot was taken is not repeated
   after it.
3. skycued pings every 15 s; a client from which nothing (no pong, no
   frame) has arrived for more than 30 s is disconnected at the next
   ping. Browsers answer pings automatically.

Messages from the client are ignored: changes go through the HTTP API.
A client that does not keep up (more than 2 MiB of messages waiting to be
sent to it) is disconnected (logged at WARN) without slowing the others;
it should reconnect and start again from a new snapshot.

## Static files

`GET /` serves `index.html`; `GET /<path>` serves the file at that path
under the www directory: `--www-dir`, by default
`<directory of skycued>/../share/skycue/www`. If that directory does not
exist, skycued logs a WARN at startup and every static path is 404; the
API still works.

- The path is percent-decoded first. A path with an empty, `.` or `..`
  segment (`/../x`, `/%2e%2e/x`, `//x`, `/a/./b`), a NUL or a backslash,
  a malformed `%` escape, or that does not start with `/`, is rejected
  with 400. The query string is ignored.
- Only regular files are served (a directory is 404). Symbolic links
  inside the www directory are followed. A file over 8 MiB is not served:
  500 `file too large to serve` (logged at WARN); one that cannot be read
  is 500 too.
- `Content-Type` by extension: `html` (`text/html; charset=utf-8`), `js`
  (`application/javascript`), `css` (`text/css`), `png`, `svg`
  (`image/svg+xml`), `json`, `ico` (`image/x-icon`), `jpg`, `webp`,
  `woff2`, `txt`; anything else `application/octet-stream`.
- `Cache-Control: max-age=86400` under `/tiles/` and `/vendor/` (map tiles,
  third-party libraries), `no-cache` for everything else (the UI's own
  html/js/css are revalidated on every load).
- `HEAD` returns the headers of `GET` without the body. Other methods: 405.

## Runtime overlay: `skycue.runtime.json`

Changes made from the web UI (filter values, allow/block list entries) are
not written to `skycue.conf`. They go to `skycue.runtime.json`, a file in
`STATE_DIR` when that is set (`/var/lib/skycue` in the packages), else in
the same directory as `skycue.conf`, which skycued reads at startup, so they
survive a restart. With `STATE_DIR` set, a `skycue.runtime.json` left next
to `skycue.conf` (from before `STATE_DIR` was set) is not read; skycued logs
a WARN naming both paths, so move it into `STATE_DIR` to keep its changes.
What skycued runs with is:

- **baseline**: `skycue.conf`, and the list files it names in
  `ALLOW_LIST_FILE` / `BLOCK_LIST_FILE` (a key left empty is an empty list);
- **overlay**: `skycue.runtime.json`, on top of the baseline;
- **effective**: each filter value from the overlay when it has one, else
  from `skycue.conf`; each list is the list file's entries, minus the
  overlay's `remove` entries, plus its `add` entries.

### Schema

```json
{
  "version": 1,
  "filters": {"max_range_nm": 42, "min_elevation_deg": 5},
  "lists": {
    "allow": {"add": ["N123AB"], "remove": ["a1b2c3"]},
    "block": {"add": ["c0ffee"], "remove": []}
  }
}
```

| Key | Type | Meaning |
|-----|------|---------|
| `version` | integer, must be `1` | format version (may be left out) |
| `filters.max_range_nm` | number > 0, or `null` | replaces `MAX_RANGE_NM` |
| `filters.min_elevation_deg` | number -90..90, or `null` | replaces `MIN_ELEVATION_DEG` |
| `lists.allow.add`, `lists.block.add` | array of entries | added to the list |
| `lists.allow.remove`, `lists.block.remove` | array of entries | removed from the list file's entries |

Every key is optional: a missing key (or `null`) means "no change from the
baseline", and a missing file is an empty overlay. Entries use the list file
grammar (six hex digits for an address, or a registration of 2-10 letters,
digits and `-`); they match ignoring case. Unknown keys are ignored with a
warning in the log.

skycued writes the file atomically (to `skycue.runtime.json.tmp`, then
renamed over it), and deletes it when the overlay becomes empty. All its
writes (this file, and `skycue.conf` and the list files on save as
baseline) work this way: a file that is a symlink stays a symlink (the
temporary file goes next to the file it points to, which is the one
replaced), and the replaced file's mode and, when skycued may set it, its
owner are kept. Adding an
entry that the overlay removes cancels the removal, and removing one that it
adds cancels the addition, so the file holds only real differences from the
baseline.

### Malformed file

A file that is not a JSON object, holds a value of the wrong type or out of
range, or a bad list entry is malformed as a whole. At startup skycued logs
an ERROR naming the problem, ignores the file (the baseline applies) and
renames it to `skycue.runtime.json.rejected` (the log names it), so a later
change from the UI does not overwrite it; fix it and rename it back, or
copy what you need from it. When the file is edited by hand while
skycued runs, a malformed edit is logged once at ERROR and the previous
overlay stays in effect.

### Save as baseline

"Save as baseline" moves the overlay into the baseline files and clears it:

- `MAX_RANGE_NM` / `MIN_ELEVATION_DEG` are rewritten in `skycue.conf` in
  place; every other byte of the file (comments, blank lines, other keys) is
  kept. A key that is not in the file is appended at the end under a
  `# added by skycue save-baseline` line. When `STATE_DIR` is set they go
  to `skycue.overrides.conf` in `STATE_DIR` instead, the same way (the file
  is created when missing), and `skycue.conf` is not touched: it may belong
  to root and skycued need not be able to write it.
- `add` entries are appended to the list files (a list file that is named
  but missing is created).
- `remove` entries are commented out in the list files, as
  `# removed by skycue save-baseline: <the line>`, so the history stays
  visible.
- A list with changes whose `ALLOW_LIST_FILE` / `BLOCK_LIST_FILE` key is
  empty cannot be saved: its changes stay in the overlay and the result says
  so. The same goes for a file that cannot be written.

This is the only time skycued writes `skycue.conf` (`STATE_DIR` unset) or
`skycue.overrides.conf` (`STATE_DIR` set; `skycue.conf` is then never
written).

`skycue.overrides.conf` has the `skycue.conf` grammar and may assign only
`MAX_RANGE_NM`, `MIN_ELEVATION_DEG`, `ALLOW_LIST_FILE` and
`BLOCK_LIST_FILE`; skycued reads it right after `skycue.conf` (only when
`STATE_DIR` is set), and its values replace `skycue.conf`'s. Any other key
in it is an error naming the file and line, as is a line that does not
parse; at startup that stops skycued, as an error in `skycue.conf` does.
A relative `ALLOW_LIST_FILE` / `BLOCK_LIST_FILE` set there is taken from
`STATE_DIR` too. You may edit it by hand, or delete it to go back to
`skycue.conf`'s values. An edit of `skycue.conf` to a key that
`skycue.overrides.conf` sets changes nothing while that file sets it.

### Hot reload

skycued checks `skycue.conf`, `skycue.overrides.conf` (when `STATE_DIR` is
set), `skycue.runtime.json` and both list files once a second; an edit of
either config file reloads both. A hand edit is applied once the file has stayed the same for two
checks in a row (so within about 2 s, and never while an editor is still
writing it), and recorded as a `filter_change` with origin
`file:<file name>`. skycued compares each file's modification time, size and
inode; on a file system whose modification times have 1 s resolution, an
edit that keeps the size and lands in the same second as the previous one
can go unnoticed (save it again, or let the editor replace the file). A `skycue.conf` edit that
does not parse is logged once at ERROR and the previous configuration stays
in effect. Only the filter keys (`MAX_RANGE_NM`, `MIN_ELEVATION_DEG`,
`ALLOW_LIST_FILE`, `BLOCK_LIST_FILE`) take effect at once; a change to any
other key that now differs from the value skycued is running with is
logged at WARN as requiring a restart (setting it back clears that).

The overlay wins over `skycue.conf`. A hand edit of `MAX_RANGE_NM` or
`MIN_ELEVATION_DEG` while the overlay holds a value for that key changes
nothing that skycued runs with, and is logged at WARN:
`runtime: MAX_RANGE_NM edited in skycue.conf but overridden by
skycue.runtime.json (use the UI or "save as baseline")`. To make the edit
take effect, clear the overlay's value (the ↺ button in the UI, or
`PUT /api/filters` with `null`), or save the running values as the
baseline first and edit afterwards.

## FIS-B

skycued decodes the FIS-B uplinks (UAT 978 MHz ground broadcasts) that
dump978 passes it, or that a replayed recording holds (`FISB_ENABLE=yes`,
the default), into a store of current products: text reports, NOTAMs and
advisories with their shapes (TWGO), NEXRAD mosaics and the other raster
layers. The routes below read that store; the WebSocket carries what
changes. Every FIS-B route is `GET` only (any other method: `405` with
`Allow: GET`); an unknown `/api/fisb/...` path is `404`; a bad query value
is `400` with the usual `{"error": ...}`. With `FISB_ENABLE=no` every route
answers as for an empty store (`radar`/`raster`: `404`).

The record bodies are the ones the fisb recording stream writes
(`skycued/src/fisb/fisb_json.cpp`, [recording-format.md](recording-format.md#fis-b-stream)),
listed under [FIS-B records](#fis-b-records); this section adds the
envelopes, the GeoJSON collection and the palettes. Times follow the data
clock (a replay of 2015 uplinks shows 2015 times and ages). Coordinates in
the records and mosaics are degrees with up to 6 decimals, points
`[lat, lon]`; **GeoJSON positions are `[lon, lat]`** as GeoJSON requires.

### `GET /api/fisb/status`

The FIS-B status object, also the `fisb` key of the state object and of the
WebSocket snapshot. Never any blocks: it is built from counts and mosaic
heads, without copying the store's records or blocks, so `/api/state` and a
WebSocket connect stay cheap however large the mosaics are. (`text` and
`shapes` copy the records; `radar` and `raster` copy only the one mosaic
asked for.) From the 2015 fixture
(`tests/fixtures/fisb/received-with-uplinks.jsonl`):

```json
{"enabled":true,"uplinks":704,"products":561,"bad_frames":0,"segments_open":0,"mosaic_blocks":671,"uplink_duplicates":0,
 "last_uplink_time":"2015-01-24T04:23:43.000Z","now":"2015-01-24T04:23:45.948Z","radar_age_s":825.9,
 "stations":[{"site_id":"00136d8b0008e332","lat":37.322702,"lon":-121.754994,"count":561,
              "last_seen":"2015-01-24T04:23:22.000Z"}],
 "radar":{"regional":{"key":"63","product":63,"name":"NEXRAD regional","product_time":"2015-01-24T04:10:00.000Z",
                      "age_s":825.9,"blocks":671,"published":"2015-01-24T04:23:43.979Z",
                      "expires":"2015-01-24T05:25:00.000Z"}},
 "rasters":[],
 "counts":{"metar":127,"taf":33,"pirep":2,"winds":35,"other":0,"twgo":96,"cancelled":0}}
```

| key | meaning |
| --- | --- |
| `enabled` | `FISB_ENABLE` |
| `uplinks`, `products`, `bad_frames`, `segments_open` | uplinks fed to the decoder, products decoded, uplinks rejected (bad hex, wrong size, overrun), segmented products waiting for segments |
| `mosaic_blocks` | raster blocks held across every mosaic |
| `uplink_duplicates` | uplinks dropped because another input delivered the same one within 2 s (two 978 feeds hearing the same ground station) |
| `last_uplink_time` | receive time of the last uplink (absent before any) |
| `now` | the data clock |
| `radar_age_s` | `now` minus the newest NEXRAD product time (absent without one) |
| `clock_skew_s` | how far the newest mosaic product time runs ahead of `now`, in seconds; absent when none is ahead. The page warns in its status strip past 600 s (a receiver or ground station clock off) |
| `stations` | ground stations heard in the last 60 min: `site_id` (16 hex digits, a stable identity derived from the station's header position; the uplink header carries no usable site number), `lat`, `lon`, `count` (products from it), `last_seen` |
| `radar.regional`, `radar.conus` | the mosaic head of product 63 / 64, **left out** when there is none (the API never sends `null`): `key`, `product`, `name`, `product_time`, `age_s`, `blocks` (count), `published` (absent until first published), `expires` |
| `rasters` | one mosaic head per raster layer (70/71/84/90/91/103; 70/71/90/91 per altitude code), as above plus `altitude_code` |
| `counts` | records held: `metar`, `taf`, `pirep`, `winds`, `other` (unclassified product-413 text), `twgo` (TWGO records not cancelled), `cancelled` (TWGO records in their 30 min cancelled window) |

### `GET /api/fisb/text`

`?type=metar|taf|pirep|winds|other|twgo` (optional; another value: `400`)
and `?station=<prefix>` (optional, case-insensitive). Without `type`, every
product-413 kind (METAR, TAF, PIREP, winds, other). `{"records": [...]}`
of [text records](#fis-b-records), sorted by station then `issued`.

`type=twgo` lists instead the TWGO records **without graphics** (text-only
NOTAMs and SUA, which have no shape and so are not in `/api/fisb/shapes`),
as TWGO records, matched on and sorted by `location`, then `received`.

```
GET /api/fisb/text?type=metar&station=KD
{"records":[{"key":"metar:KDAG","kind":"metar","station":"KDAG","issued":"2015-01-24T03:50:00.000Z",
  "expires":"2015-01-24T05:50:00.000Z","text":"METAR KDAG 240350Z AUTO 26006KT 10SM CLR 09/M10 A3026 RMK AO2 SLP247\n T00941100="},
 {"key":"metar:KDRA",...},{"key":"metar:KDVO",...}]}
```

### `GET /api/fisb/shapes`

`?product=8,11,12` (optional: a comma-separated list of product IDs; a
value that is not one: `400`). A GeoJSON `FeatureCollection` with one
`Feature` per shape of every TWGO record that is not cancelled (a record
with three shapes gives three features sharing `properties.key`):

```json
{"type":"FeatureCollection","features":[
 {"type":"Feature","id":"8-5-4229-KCCR-1/0",
  "geometry":{"type":"Point","coordinates":[-122.056732,37.989349]},
  "properties":{"key":"8-5-4229-KCCR-1","product":8,"name":"NOTAM","report":4229,"location":"KCCR","label":"KCCR",
                "text":"NOTAM-FDC KCCR.5/4229 152022Z !FDC 5/4229 CCR IAP BUCHANAN FIELD, ...","text_end":"2015-07-14T20:22:00.000Z",
                "expires":"...","cancelled":false,"pending":false,"geom":"point","alt_low_ft":0,"alt_high_ft":0,
                "agl":true,"shape":0}}]}
```

| `properties.geom` | `geometry` |
| --- | --- |
| `polygon` | `Polygon`, one closed ring (a ring of fewer than 3 distinct points becomes a `LineString` or `Point`) |
| `polyline` | `LineString` (G-AIRMET freezing levels) |
| `point` | `Point` |
| `circle` | `Point` at the centre; the radius is `properties.radius_nm` |
| `prism` | `Polygon`: a 32-sided ring of `radius_nm` around the bottom centre; `properties.center` is that centre `[lon, lat]`, `radius_nm` the radius |

`properties`: `key`, `product`, `name`, `report`, `location`, `label`?,
`text`? (the record's text: the report, absent for G-AIRMET), `start`?,
`graphics_end`?, `text_end`?, `expires`, `cancelled`, `pending`, `geom`,
`radius_nm`? (circle, prism), `center`? (prism), `alt_low_ft`?,
`alt_high_ft`?, `agl` (altitudes above ground), `shape` (index in the
record). The fields mean what they do in the [TWGO record](#fis-b-records).
`id` is `<key>/<shape>`.

The collection is not paged or limited: by design it is everything the
store holds, and the store is what bounds it (TWGO records expire at their
end times and, but for SUA, 70 min after their last broadcast; a ground station sends at
most a few hundred).

### `GET /api/fisb/radar`

`?product=regional|conus` (or `63`/`64`; missing or another value: `400`).
The [mosaic](#fis-b-records) with every block, plus `palette` and `legend`;
`404` when there is no such mosaic. The mosaic is the store's current one,
published or not (`published` is absent until the store first publishes
it: 10 s after its last change, or every 30 s while changes keep
coming; a block identical to the one held is no change, so an unchanged
mosaic re-sent every cycle is not republished); a client normally fetches it
on a `fisb_radar` item.

```
GET /api/fisb/radar?product=regional
{"key":"63","product":63,"name":"NEXRAD regional","scale":0,"product_time":"2015-01-24T04:10:00.000Z",
 "updated":"2015-01-24T04:20:19.000Z","published":"2015-01-24T04:23:43.979Z","expires":"2015-01-24T05:25:00.000Z",
 "bins_per_block":128,"block_count":671,"evicted":0,
 "blocks":[{"block":229348,"lat0":34,"lon0":-121.6,"dlat":0.016667,"dlon":0.025,"rle":[[0,128]]}, ...],
 "palette":[{"level":0,"dbz_max":5,"color":"transparent"},{"level":1,"dbz_min":5,"dbz_max":20,"color":"#04e9e7"}, ...],
 "legend":{"title":"NEXRAD regional reflectivity","units":"dBZ","approximate":false,
           "levels":[{"level":0,"label":"< 5 dBZ"},{"level":1,"label":"5-19 dBZ (very light)"}, ...]}}
```

Painting: expand each block's `rle` (`[value, count]` runs, 128 bins in
total) into 4 rows of 32 bins from its NW corner (`lat0`, `lon0`), each bin
`dlat` high and `dlon` wide, W->E then N->S; paint bin value `v` with
`palette[v].color` (`transparent`: leave it).

The NEXRAD `palette`: one entry per level 0-7, index = level, each
`{"level", "dbz_min"?, "dbz_max"?, "color"}` (integers, and a CSS colour or
`transparent`), meaning `dbz_min <= dBZ < dbz_max`; an absent bound is open
(level 0 has no `dbz_min`, level 7 no `dbz_max`; CONUS level 0 has neither
and CONUS level 1 only `dbz_max`). Colours are the NWS reflectivity scale's
colour at each level's lower bound:

| level | regional 63 | CONUS 64 | colour |
| --- | --- | --- | --- |
| 0 | < 5 dBZ | no data | `transparent` |
| 1 | 5-19 dBZ, very light | < 20 dBZ, or no precipitation (every empty CONUS block is level 1) | `#04e9e7` (regional), `transparent` (CONUS) |
| 2 | 20-29, light to moderate | same | `#02fd02` |
| 3 | 30-39, moderate | same | `#008e00` |
| 4 | 40-44, moderate to heavy | same | `#fdf802` |
| 5 | 45-49, heavy | same | `#fd9500` |
| 6 | 50-54, very heavy | same | `#fd0000` |
| 7 | >= 55, extreme | same | `#f800fd` |

`legend`: `{"title", "units": "dBZ", "approximate": false, "levels": [{"level",
"label", "approximate"?}]}`; the CONUS levels 0 and 1 carry
`"approximate": true` (their meaning is from one reference implementation,
docs/fisb-reference.md §5.3-5.4).

### `GET /api/fisb/raster`

`?product=70|71|84|90|91|103` (another value, 63/64 included: `400`) and
`?altitude=<code>` (optional, 0-7; else `400`): the layer of that product
(and altitude code, for 70/71/90/91), in the same shape as a radar mosaic
with a product-specific `palette` and `legend`; without `altitude`, the
lowest altitude code held. `404` when there is no such layer. Bin values
are passed through as decoded (docs/fisb-reference.md §5.5); every mapping
below comes from one reference implementation (FD) and is not verified
against a specification, so every raster legend has `"approximate": true`.

| product | value | legend `levels` | palette |
| --- | --- | --- | --- |
| 103 lightning | 0-15: polarity (8 = positive) + strike-count code (0 none, 1, 2, 3-5, 6-10, 11-15, > 15, 7 no data) | 16 levels: `"3-5 strikes (positive)"`, ... | codes 1-6 `#fff5a0` -> `#ff0000`; 0 and 7 `transparent` |
| 90/91 turbulence | 0-15 EDR bins: 0 < 0.07, n = 0.07n-0.07n+0.06, 14 >= 0.98, 15 no data | 16 levels | 1-14 `#ffffb2` -> `#b10026`; 0, 15 `transparent` |
| 70/71 icing | bit-packed `ddsssppp`: SLD (bits 7-6), severity (5-3), probability (2-0) | the severity field (none, trace, light, moderate, severe, heavy, unused, no data); `legend.fields` describes all three (`name`, `shift`, `mask`, `levels`); the probability table's order is doubtful and its levels are marked approximate | indexed by severity, `(value >> 3) & 7` (`legend.palette_field: "severity"`): 1-5 `#c6dbef` -> `#54278f`; 0, 6, 7 `transparent` |
| 84 cloud tops | 0-15; the level-to-altitude mapping is not known | `"level 0"` ... `"level 15"` | 1-15 `#d0e8ff` -> `#08306b`; 0 `transparent` |

A raster `palette` entry is `{"level", "color"}` (no dBZ bounds). The
raster `legend` is `{"title", "units"?, "note"?, "approximate": true,
"levels": [{"level", "label", "approximate"?}]}`, with `units` for
lightning (`"strikes per bin"`) and turbulence (`"EDR"`) only, `note` for
lightning, icing and cloud tops, and for icing also `packed`,
`palette_field` and `fields`.

The colours between the two ends are linear in RGB. Layers 70/71/90/91 add
`legend.altitude_ft` (low products 70/90: `code * 2000 + 2000`; high
71/91: `code * 2000 + 18000`) with `legend.altitude_approximate: true`
(docs/fisb-reference.md §5.1).

```
GET /api/fisb/raster?product=70        (tg30.978, 2020-10-30)
{"key":"70-0","product":70,"name":"icing low","altitude_code":0,"scale":1,"product_time":"2020-10-30T08:00:00.000Z",
 ..., "bins_per_block":128,"block_count":55,"evicted":0,"blocks":[...],
 "palette":[{"level":0,"color":"transparent"},{"level":1,"color":"#c6dbef"}, ...],
 "legend":{"title":"Icing (low)","packed":"ddsssppp","palette_field":"severity","note":"...",
           "fields":[{"name":"severity","shift":3,"mask":7,"levels":[...]},{"name":"sld","shift":6,"mask":3,"levels":[...]},
                     {"name":"probability","shift":0,"mask":7,"levels":[{"level":0,"label":"<= 5%","approximate":true}, ...]}],
           "approximate":true,"altitude_ft":2000,"altitude_approximate":true,
           "levels":[{"level":0,"label":"none"},{"level":1,"label":"trace"}, ...]}}
```

### `GET /api/fisb/locations`

Positions for the location ids the FIS-B text products name (`KPRG`,
`KCVG`; METARs, TAFs, winds and PIREPs carry an id but no coordinates).
These are **locations**, not **stations**: `stations` in the status object
are the FIS-B ground stations that transmit the uplinks; a location is an
airport or navaid a report is about.

```
GET /api/fisb/locations
{"KCVG":[39.048157,-84.667511],"KPRG":[39.699097,-87.670212]}
```

One `[lat, lon]` per id, ids sorted; `{}` when none is known. Two sources,
merged:

- **Learned** from the TWGO records with graphics (NOTAM-D/TFR points,
  circle and prism centres averaged; a small polygon's centroid while the
  id has no point), for airport-style ids only (4 characters `[A-Z0-9]`:
  `K???`, `C???`, `PA??`, `PH??`). Kept in `fisb-locations.json` next to
  `skycue.conf` (saved at most once a minute when it changed, and at
  shutdown), so coverage accumulates across restarts.
- **Operator** rows from `LOCATIONS_FILE` (a CSV `id,lat,lon`), read at
  startup. A learned position wins over an operator row for the same id;
  operator rows are never written to `fisb-locations.json`.

The page keeps this map and applies `fisb_location` items to it.

### FIS-B WebSocket items

Items of the `{"type": "events"}` messages, in the order the store
produced them. The snapshot never carries FIS-B products, only the status
object: a client that wants them GETs the routes above after the snapshot
and then applies these items.

```json
{"type":"fisb_text","product":413,"key":"metar:KSFO","record":{ ...text record... }}
{"type":"fisb_text","product":13,"key":"13-15-9294--1","record":{ ...TWGO record without graphics... }}
{"type":"fisb_shape","product":8,"key":"8-5-4229-KCCR-1","features":[ ...Features as in /api/fisb/shapes... ]}
{"type":"fisb_radar","product":63,"key":"63","name":"NEXRAD regional","product_time":"2015-01-24T04:10:00.000Z",
 "blocks":671,"expires":"2015-01-24T05:25:00.000Z"}
{"type":"fisb_raster","product":70,"key":"70-3","name":"icing low","altitude_code":3,"product_time":"...","blocks":55,"expires":"..."}
{"type":"fisb_expired","kind":"text","product":413,"key":"metar:KSFO","station":"KSFO","reason":"expired","expires":"..."}
{"type":"fisb_expired","kind":"twgo","product":8,"key":"8-15-12045-KSCK-1","reason":"cancelled","expires":"..."}
{"type":"fisb_location","id":"KPRG","lat":39.699097,"lon":-87.670212}
```

- `fisb_text`: a product-413 report is new or replaced (`record` is the
  text record, with `kind`), or a TWGO record without graphics is new or
  changed (`record` is the TWGO record, `kind: "twgo"`; the same records as
  `/api/fisb/text?type=twgo`). Replace the record with the same `key`. A
  TWGO record arriving here has no graphics (`has_graphics: false`): when
  its text was amended the store dropped the old graphics, so remove every
  feature drawn for that `key` (the page does).
- `fisb_shape`: a TWGO record with graphics is new or changed: one item
  per record, carrying all its features (each with the record's text in
  `properties`). Replace every feature whose `properties.key` is `key`.
- `fisb_radar` / `fisb_raster`: a mosaic was published. A **notification
  only**: `product`, `key`, `name`, `altitude_code` (raster layers that
  have one), `product_time`, `blocks` (the block count), `expires`; never
  the blocks. GET `/api/fisb/radar?product=<63|64>` or
  `/api/fisb/raster?product=<id>&altitude=<code>` for them.
- `fisb_expired`: an item left the store (`reason` `expired`), or a TWGO
  record was cancelled (`cancelled`). `kind` is `text`, `twgo`, `radar` or
  `raster`; `key` is the item's key (the records' `key`, a mosaic's
  `key`); `station` is added for text. `kind` comes from the store (which item
expired), not from parsing `key`. Remove the item (for `twgo`, every
  feature with that `properties.key`).
- `fisb_location`: a location id was learned for the first time, or its
  learned position is now 1e-4 degrees or more (latitude or longitude)
  from the position last announced for it (or, for an id loaded at
  startup, from the one `GET` gave); small moves add up. It follows the
  `fisb_shape` that taught it. Set `id` to `[lat, lon]` in the map from
  `GET /api/fisb/locations`. Not recorded.

**Recording and WebSocket names.** The fisb recording stream and the
WebSocket use the same record bodies under these types:

| recording record `type` | WebSocket item `type` |
| --- | --- |
| `fisb_text` | `fisb_text` (`record` = the text record) |
| `fisb_twgo` (with graphics) | `fisb_shape` (the record as `features`) |
| `fisb_twgo` (without graphics) | `fisb_text` (`record` = the TWGO record) |
| `fisb_radar`, `fisb_raster` (the mosaic with every block) | `fisb_radar`, `fisb_raster` (the head only; `blocks` is the count) |
| `fisb_expired` (`key`, `product`, `reason`, `expires`) | `fisb_expired` (the same plus `kind`, `station`) |

### FIS-B records

The objects the recording and the API share. The conventions above apply:
times are ISO-8601 UTC with milliseconds, absent values are left out.
Coordinates are degrees with up to 6 decimals; points are `[lat, lon]`.

**Text record** (product 413: METAR/SPECI, TAF, PIREP, winds aloft):

| key | type | meaning |
| --- | --- | --- |
| `key` | string | store key: `metar:KSFO`, `taf:KSFO`, `winds:SFO:6`, `pirep:<station>:<issued epoch s>:<fnv1a hex of text>` |
| `kind` | string | `metar`, `taf`, `pirep`, `winds`, `other` |
| `station` | string | first 3-4 character station token (may be empty) |
| `issued` | time | the report's `ddhhmmZ` group, else the APDU time; winds: the APDU (product issue) time |
| `valid` | time | winds only: the forecast valid time (the report's `ddhhmmZ` group, e.g. `WINDS TUL 301800Z`) |
| `expires` | time | when the store drops it (METAR, PIREP +120 min; TAF end of validity; winds `valid` +12 h unless the next issue of the period replaces it) |
| `period_h` | integer | winds only: forecast period, 6, 12 or 24 |
| `text` | string | the report |

**TWGO record** (8 NOTAM, 11 AIRMET, 12 SIGMET, 13 SUA, 14 G-AIRMET, 15 CWA, 16/17 NOTAM-TRA/TMOA):

| key | type | meaning |
| --- | --- | --- |
| `key` | string | `pid-year-report-location-month`, e.g. `8-15-12045-KSCK-1` |
| `kind` | string | always `twgo` (a text record's `kind` is `metar`, `taf`, ...: so a `fisb_text` item's `record` says which it is) |
| `product`, `name` | integer, string | product ID and display name |
| `report` | integer | report number |
| `location` | string | TWGO header location (may be empty) |
| `label` | string | object label, when alphanumeric |
| `text` | string | the report text (absent for G-AIRMET, which has none) |
| `has_graphics`, `complete`, `cancelled` | boolean | graphics half present; complete (text present, or graphics for 14); cancelled (text and shapes cleared, kept 30 min) |
| `pending` | boolean | not yet in force: `start` is still ahead of the data clock. The record is published again with `pending: false` when the clock reaches `start` |
| `start`, `graphics_end` | time | graphics applicability window (from the graphics half) |
| `text_end` | time | NOTAMs (8, 16, 17): the end of validity in the text's `yymmddhhmm-yymmddhhmm` group; absent for `PERM` or no group |
| `received`, `last_received`, `expires` | time | first and last reception (a TFR keep-alive or a repeat counts); expiry: the record's end is the later of `text_end` and `graphics_end`; NOTAM, AIRMET, SIGMET, G-AIRMET and CWA expire at the earlier of that end and 70 min after the last reception, SUA at its end; with no end, 70 min after the last reception. A report whose year is not the data clock's (the neighbouring year allowed across New Year) is never stored |
| `shapes` | array | `{"geom": "polygon"\|"polyline"\|"point"\|"circle"\|"prism", "points": [[lat,lon],...], "radius_nm"?, "alt_low_ft"?, "alt_high_ft"?, "agl": bool}` |

**Mosaic** (63/64 NEXRAD; 70/71/84/90/91/103 rasters, levels passed through):

| key | type | meaning |
| --- | --- | --- |
| `key` | string | `63`, `64`, `84`, ...; `70-3` for a layer with an altitude code |
| `product`, `name` | integer, string | product ID and display name |
| `altitude_code` | integer | 70/71/90/91: the raw 3-bit altitude code |
| `scale` | integer | scale factor of the blocks (0 high, 1 medium, 2 low; each block also carries its own size) |
| `product_time` | time | newest APDU hh:mm merged; `expires` is 75 min (NEXRAD, lightning) or 105 min after it |
| `updated`, `published` | time | last change merged (an identical repeat does not count); last publish |
| `bins_per_block` | integer | 128 |
| `block_count`, `evicted` | integer | blocks held; blocks evicted over the 4096 cap |
| `blocks` | array | `{"block", "lat0", "lon0", "dlat", "dlon", "rle": [[value,count],...]}`: NW corner, one bin's size, and the 128 bins W->E then N->S in 4 rows of 32. Empty CONUS blocks are level 1, empty regional blocks level 0 |

**Expired**: `{"key", "product", "reason": "expired"|"cancelled", "expires"}`.

**Summary** (`fisb::summaryJson`): `{"enabled", "text_records", "twgo_records",
"mosaics", "mosaic_blocks", "stations", "radar_age_s"?}`; `radar_age_s` is the
data clock minus the newest NEXRAD product time.

## Statistics

skycued keeps a statistics history (the stats view design, §4):
per-minute (last 24 h) and per-hour (last 30 days) series, histograms,
range coverage, a position grid, a signal-versus-range matrix, categorical
counts and a ledger of every aircraft heard (at most `STATS_AIRCRAFT_MAX`,
the 5 % heard longest ago forgotten past it, counted in `evicted`). It is
kept in `STATS_FILE` ([operating.md](operating.md)). The routes below read
it; the WebSocket carries nothing of it (the Stats view polls). Every route
but `reset` is `GET` only (any other method: `405` with `Allow: GET`); an
unknown `/api/stats/...` path is `404`; a bad query value is `400` with the
usual `{"error": ...}`. `GET /api/stats` (no slash) is the engine's own
counters and is not part of this: a reset leaves it alone.

**Clock.** Every time here is the data clock (the newest observation's time
plus the time since; the wall clock before any), so a replay of an old
recording builds a history in that recording's time. `since` is when this
history began (the first value stored, or the last reset); `series_since`
is when the series began. The series restart when an observation comes
in more than one bucket older than the newest bucket (a replay after a
live run, say): both rings are emptied and `series_since` moves to that
time, while the histograms, coverage, grid, counts and ledger keep
counting, so `series_since` can be later than `since`. Right after a
restart or a load, an aircraft already counted in the open minute or hour
may be counted in that bucket's `aircraft` once more.

**What follows the time range.** `summary`'s `range` object (totals from
the series, and `aircraft`, `by_class`, `by_country` from the ledger
entries whose `last_seen` is in the range), `series` and the aircraft
table's own `last_seen` follow `from`/`to`. The histograms (`hist`),
`counts`, `coverage`, the grid and `rssi` are whole-history since `since`.

**Cost.** Loading a full file at startup takes about 150 MB of memory for
a moment (PropertyTree); the file is written at every `STATS_SAVE_S` while
changed, at a clean stop and at once on a reset, on the io thread (about
half a second at the defaults in a Debug build). The responses at the
defaults with a full store (20,000 aircraft, 40,000 grid cells, both rings
full, 16 inputs): `summary` about 2 KB, `series` 170 KB (minute) / 90 KB
(hour), `coverage` 780 KB (nearly all of it the grid), `rssi` 4 KB, an
aircraft page of 100 rows 72 KB (1000 rows: 720 KB).

### Time range: `from`, `to`

`summary` and `series` take `from` and `to`, ISO-8601 UTC as everywhere
(`2026-11-03T12:00:00Z`, with or without milliseconds; anything else:
`400`). `to` defaults to the data clock; `from` to `since` (or
`series_since` when it is earlier; the oldest minute bucket when neither is
set). `from` later than `to` is an empty range: `200` with zero totals and
empty columns. A bucket is in the range when any of it is (`start` + its
length > `from` and `start` <= `to`); a ledger entry when its `last_seen`
second is in [`from`, `to`].

### `GET /api/stats/summary`

```json
{"since":"2026-11-03T12:00:00.000Z","series_since":"2026-11-03T12:00:00.000Z","now":"2026-11-03T14:10:02.512Z",
 "receiver":{"lat":38.9,"lon":-77},"aircraft_total":412,"evicted":0,"uat_seen":true,
 "range":{"from":"2026-11-03T12:00:00.000Z","to":"2026-11-03T14:10:02.512Z","res":"minute",
          "observations":98211,"positions":90310,"new_aircraft":412,"sent_updates":20481,"drops":388,
          "emergencies":0,"max_range_nm":231.4,"max_range_key":"a1b2c3|icao","aircraft":412,
          "by_class":{"airline":301,"military":4,"private":77,"unknown":30},
          "by_country":[{"name":"United States","n":371},{"name":"Canada","n":22},{"name":"unknown","n":11},...]},
 "hist":{"range":{"start":0,"bin":10,"counts":[...50]},"altitude":{...},"speed":{...},
         "vertical_rate":{...},"elevation":{...},"rssi":{...}},
 "counts":{"by_category":{"A3":51020,"none":30122,...},"by_source":{"adsb_1090":90122,"uat_978":8089},
           "by_addr_type":{"icao":97000,"tisb_trackfile":1211},"by_input":{"readsb":90122,"dump978":8089},
           "by_decision":{"send":88000,"out_of_range":10211},"by_squawk_emergency":{}}}
```

| key | meaning |
| --- | --- |
| `since`, `series_since` | see **Clock** above; each absent when the store is empty (never set) |
| `now` | the data clock |
| `receiver` | `{lat, lon}` of the receiver; absent without receiver coordinates (then range, elevation, coverage and the signal matrix stay empty) |
| `aircraft_total` | ledger entries now (all time) |
| `evicted` | ledger entries forgotten to keep the ledger at `STATS_AIRCRAFT_MAX` |
| `uat_seen` | a UAT observation, frame or FIS-B product has ever been counted (the page shows the UAT charts only then) |
| `range.from`, `range.to` | the range used, defaults filled in |
| `range.res` | the series the totals come from: `minute` when `to` − `from` <= 24 h, else `hour` |
| `range.observations`, `positions`, `new_aircraft`, `sent_updates`, `emergencies` | sums over the buckets in range (see `series` for each) |
| `range.drops` | `drops_timeout` + `drops_filtered` + `drops_source_lost` over those buckets (the drops sent) |
| `range.max_range_nm`, `max_range_key` | the largest bucket maximum range in range and the aircraft key that made it; both absent when none |
| `range.aircraft` | ledger entries with `last_seen` in range |
| `range.by_class` | those entries by class, always the four keys `airline`, `private`, `military`, `unknown`: the page's rule (`www/app_pure.js` `trackClass`) on the entry's newest callsign, category, registration and address |
| `range.by_country` | those entries by the State their ICAO address is allocated to (`unknown`: a non-ICAO address or none), `[{name, n}]` most first, ties by name; the first 20, then `{"name":"other","n":<the rest>}` when there are more |
| `hist.<name>` | `{start, bin, counts}`: `counts[i]` is the observations with a value in [`start` + i·`bin`, `start` + (i+1)·`bin`); below the first edge counts in the first bin, at or above the last edge in the last. `range` 0, 10 nm, 50 bins; `altitude` 0, 1000 ft, 60 (barometric, else geometric; 0 on the ground); `speed` 0, 20 kt, 40 (ground speed); `vertical_rate` −8000, 500 ft/min, 32; `elevation` 0, 2°, 45; `rssi` −50, 1 dB, 50. Whole history |
| `counts.by_category` | observations by emitter category (`A0`..`D7`; `none` without one or another value) |
| `counts.by_source` | by source: `adsb_1090`, `uat_978`, `tisb`, `adsr`, `mlat`, `other` |
| `counts.by_addr_type` | by address type: `icao`, `non_icao`, `tisb_trackfile`, `other` |
| `counts.by_input` | by input id (at most 16 ids, the rest under `other`; `unknown` without one) |
| `counts.by_decision` | by the filter decision in force: `send`, `blocked`, `allow_listed`, `out_of_range`, `below_elevation`, `no_position` |
| `counts.by_squawk_emergency` | observations squawking `7500`, `7600`, `7700` (only those seen) |

A count object holds only the names seen. Every count is whole-history.

### `GET /api/stats/series`

`?res=minute|hour` (default `minute`; anything else `400`), `from`, `to`.
The buckets in range, oldest first, as columns of one length:

```json
{"res":"minute","bucket_s":60,"t":["2026-11-03T14:08:00.000Z","2026-11-03T14:09:00.000Z","2026-11-03T14:10:00.000Z"],
 "observations":[812,790,133],"positions":[744,731,120],"aircraft":[61,59,40], ... ,
 "max_range_nm":[188.2,201.7,0],"max_range_key":["a1b2c3|icao","c0ffee|icao",null],
 "by_input":{"dump978":[70,66,11],"readsb":[742,724,122]}}
```

| key | meaning |
| --- | --- |
| `res`, `bucket_s` | `minute` / 60 or `hour` / 3600 |
| `t` | each bucket's start time. Buckets are contiguous (a bucket with nothing in it is all zeros) |
| `observations` | observations |
| `positions` | observations with a position |
| `aircraft` | distinct aircraft (address and address type) heard in the bucket |
| `new_aircraft` | aircraft not in the ledger when heard (a forgotten one heard again is new again) |
| `sent_updates` | updates sent on the cue stream |
| `drops_timeout`, `drops_filtered`, `drops_source_lost` | drops sent, by reason |
| `drops_blocked`, `drops_out_of_range`, `drops_below_elevation`, `drops_no_position` | the filtered drops by the decision that caused them |
| `uat_downlink`, `uat_uplink` | dump978 frames (the increase of the engine counters of the same names) |
| `fisb_products` | FIS-B products decoded |
| `emergencies` | observations with an emergency status other than `none`, or squawking 7500/7600/7700 |
| `tcp_clients`, `web_clients` | cue-stream TCP clients and web UI WebSocket clients: in a minute the last sample, in an hour the largest |
| `max_range_nm`, `max_range_key` | the largest range heard in the bucket and the aircraft key; `0` and `null` when none |
| `by_input` | `{<input id>: [observations per bucket]}` for every input seen in the range (`0` where it had none) |

### `GET /api/stats/coverage`

```json
{"sector_deg":5,"bands":["0-2.5k","2.5-5k","5-10k","10-20k","20-30k","30-40k","40k+","unknown"],
 "cells":[{"sector":0,"band":4,"max_range_nm":143.8,"key":"a1b2c3|icao","time":"2026-11-03T13:02:11.000Z","n":2210},...],
 "grid":{"cell_deg":0.05,"cap":40000,"full":false,"cells":[[38.925,-76.975,311],[38.975,-76.975,90],...]},
 "receiver":{"lat":38.9,"lon":-77}}
```

| key | meaning |
| --- | --- |
| `sector_deg` | 5: sector `s` holds bearings [5s, 5s + 5)° from the receiver (initial great-circle bearing, true north) |
| `bands` | the 8 altitude band names, indexed by `band`: ground (and below 2500 ft) is band 0; `unknown` without an altitude |
| `cells` | every sector × band cell with observations, sector-major: `sector` 0..71, `band` 0..7, `max_range_nm` the largest range heard there, `key` the aircraft that made it and `time` when, `n` the observations in it. Needs receiver coordinates |
| `grid.cell_deg` | 0.05: the grid's cells are 0.05° of latitude by 0.05° of longitude |
| `grid.cap`, `grid.full` | at most `cap` cells are kept; once `full`, positions in new cells are not counted (existing cells keep counting) |
| `grid.cells` | `[lat, lon, n]` per cell with positions: the cell's centre and the observations with a position in it, south to north then west to east. Needs no receiver |
| `receiver` | as in `summary` |

Whole history.

### `GET /api/stats/rssi`

```json
{"range_bin_nm":10,"rssi_bin_db":2,"rssi_start":-50,"counts":[[0,0,3,...25],...50]}
```

`counts[r][b]` is the observations with a range in [10r, 10r + 10) nm
(the last row: 490 nm and beyond) and a signal level in
[−50 + 2b, −48 + 2b) dB (clamped to the first and last column). 50 rows of
25. Needs receiver coordinates and a signal level. Whole history.

### `GET /api/stats/aircraft`

A page of the ledger.

| parameter | meaning |
| --- | --- |
| `sort` | the field to sort by: `first_seen`, `last_seen` (default), or any numeric row field (`observations`, `positions`, `sessions`, `sent_updates`, `drops`, `emergencies`, `max_range_nm`, `max_range_time`, `max_range_lat`, `max_range_lon`, `max_range_bearing_deg`, `min_alt_ft`, `max_alt_ft`, `max_gs_kt`, `min_rssi`, `max_rssi`); another value `400` |
| `dir` | `desc` (default) or `asc`; another value `400`. Entries without the sort value come last either way; ties are in `key` order, so pages are stable |
| `limit` | rows per page, default 100, clamped to 1..1000; not a whole number `400` |
| `offset` | rows to skip, default 0; not a whole number >= 0 `400`. Past the end: no rows |
| `q` | only the entries with this text (case ignored) in the address (`hex`), any of the `callsigns` or the `registration` |

```json
{"total":1,"offset":0,"limit":100,"evicted":0,
 "rows":[{"key":"a1b2c3|icao","hex":"a1b2c3","addr_type":"icao","first_seen":"2026-11-03T12:01:07.000Z",
          "last_seen":"2026-11-03T14:09:58.000Z","observations":5120,"positions":4980,"sessions":2,"sent_updates":1022,
          "drops":2,"emergencies":0,"max_range_nm":143.8,"max_range_time":"2026-11-03T13:02:11.000Z",
          "max_range_lat":40.81,"max_range_lon":-75.62,"max_range_bearing_deg":28.6,"min_alt_ft":1200,
          "max_alt_ft":36000,"max_gs_kt":481.5,"min_rssi":-31.2,"max_rssi":-4.5,"callsigns":["UAL123"],
          "squawks":["3301"],"inputs":["readsb"],"registration":"N123AB","category":"A3","source":"adsb_1090",
          "class":"airline","country":"United States"}]}
```

`total` is the entries matching `q` (all of them without); `evicted` as in
`summary`. A row (absent values left out):

| key | meaning |
| --- | --- |
| `key` | `<hex>\|<addr_type>`, the page's track key |
| `hex`, `addr_type` | the 24-bit address (6 lower-case hex digits) and its type (`icao`, `non_icao`, `tisb_trackfile`, `other`) |
| `first_seen`, `last_seen` | the first and the newest observation, whole seconds |
| `observations`, `positions` | observations, and those with a position |
| `sessions` | visits: 1, plus one for every observation more than 10 min after the previous one |
| `sent_updates`, `drops` | updates and drops (any reason) sent for it on the cue stream |
| `emergencies` | observations with an emergency (as in `series`) |
| `max_range_nm`, `max_range_time`, `max_range_lat`, `max_range_lon`, `max_range_bearing_deg` | the farthest it was heard, when, where and its bearing from the receiver; absent until it has been heard with a range |
| `min_alt_ft`, `max_alt_ft` | lowest and highest altitude (0 on the ground) |
| `max_gs_kt` | highest ground speed |
| `min_rssi`, `max_rssi` | weakest and strongest signal, dBFS |
| `callsigns`, `squawks`, `inputs` | the last 4 distinct values, newest last |
| `registration`, `category` | the newest registration and emitter category |
| `source` | the source of its newest observation |
| `class` | `airline`, `private`, `military` or `unknown`, as in `summary` |
| `country` | the State its ICAO address is allocated to; absent for a non-ICAO address or an unallocated one |

### `GET /api/stats/aircraft/{hex}`

One row as above. `{hex}` is 6 hex digits (either case), else `400`.
`?addr_type=icao|non_icao|tisb_trackfile|other` picks the entry of that
type (another value `400`); without it, the `icao` entry, else the first
of `non_icao`, `tisb_trackfile`, `other`. `404` when there is none.

### `POST /api/stats/reset`

Body `{}` (any other body `400`), with the same guard as every change
([Security](#security): `415` without the JSON `Content-Type`, `403` from
another origin). Empties the whole history (series, histograms, coverage,
grid, counts, ledger, `evicted`) and sets `since` and `series_since` to the
data clock; `STATS_FILE` is rewritten at once, and skycued logs
`stats: reset by ui <client address:port>` at INFO. Answers
`{"ok":true}` once done. Another reset within 5 s is `429`
`{"error":"the statistics were reset less than 5 s ago"}` and changes
nothing. Any other method is `405` with `Allow: POST`. The engine's
counters (`GET /api/stats`, the state object's `stats`) are not touched:
they are the process's lifetime counts.
