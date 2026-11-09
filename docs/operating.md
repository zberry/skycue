# Operating skycue

How to run skycued beside the decoders you already have, replay recordings,
and watch the cue stream. The Debian and RHEL packages and the plain CMake
install run it as the systemd unit `skycued`, with `/etc/skycue/skycue.toml`
and its files under `/var/lib/skycue` ([installing.md](installing.md)); from a
source tree it runs straight from the build tree with
`skycued --config ./skycue.toml --www-dir www` ([Run it from the build
tree](installing.md#run-it-from-the-build-tree)). Where the two shapes keep
their files differently, [Where the files are](#where-the-files-are) says so.

## The chain

```
RTL-SDR 978 MHz -> dump978-fa --raw--> readsb <- RTL-SDR 1090 MHz
                       |                 |
                       | raw UAT         | JSON (inputs.readsb.json_port)
                       v                 v
                       +---------> skycued ---> cue clients (TCP / UDP)
                                         \---> recording.dir (received, sent)
```

`dump978-fa` decodes 978 MHz UAT and serves raw frames; `readsb` decodes
1090 MHz, merges the UAT traffic it reads from dump978-fa, and serves one
JSON line per aircraft update. `skycued` reads that JSON, keeps a track
table, filters it (range, elevation, curated lists), decides when each
aircraft is worth a new cue, and forwards cues in the skycue wire protocol
([protocol.md](protocol.md)). It also reads dump978-fa's raw port, but only
to count UAT uplinks.

## Start, stop, status

skycued is the only process skycue itself runs. The decoders (readsb for
1090 MHz, dump978-fa for 978 MHz) are yours; skycued connects to their ports
and reconnects when they restart.

Installed from a package (see [installing.md](installing.md)):

```sh
sudoedit /etc/skycue/skycue.toml    # at least receiver.lat, receiver.lon
sudo systemctl enable --now skycued
systemctl status skycued            # running or not, pid, last log lines
journalctl -u skycued -f            # the log
sudo systemctl stop skycued
```

From a source tree, run it from the build tree and stop it with Ctrl-C:

```sh
build/skycued/skycued --config ./skycue.toml --www-dir www
```

A config with an unknown key or a malformed line stops skycued with exit
status 2 and the reason on stderr (or in the journal). Nothing else is
started on your behalf: bring up readsb and dump978-fa first, or let
skycued retry until they are there. The web UI is at
`http://<web.bind>:<web.port>/` (none with `web.port = 0`).

### Testing the decoder connections: skycued --check-inputs

```sh
skycued --config skycue.toml --check-inputs
```

connects once to each TCP input and prints one line per input, so decoder
ports can be tested before the service is enabled:

```
readsb 127.0.0.1:30047: connected
dump978 127.0.0.1:30978: Connection refused
ps 192.168.1.20:4000: timed out
pu: not checked (listens on 0.0.0.0:30999)
```

It checks readsb (unless `--no-readsb` or it is disabled in the inputs
file), dump978 (when `inputs.dump978.enable` is on and not `--no-uat`) and every
enabled `tcp_connect` input of the inputs file, using `inputs.readsb.host`,
`inputs.dump978.host` and the ports from the config (`--readsb-host` and
`--dump978-host` win). Enabled `udp` and `tcp_listen` inputs wait for data
and are listed as not checked. The exit status is 0 when everything checked
connected, 1 when any failed, 2 for a usage or config error (including
`--replay` together with `--check-inputs`). Nothing else starts: no
listeners, no recordings, no state files.

Each check is one connection attempt through the same network library the
daemon uses (no retry), so a listener that accepts and closes at once still
counts as connected. It gives up after 3 s, and all run at once. A host name lookup
cannot be cancelled, so one that is still running at that point is reported
as `timed out` and abandoned; it ends on the system resolver's own timeout,
after skycued has exited.

### Running without a 1090 or 978 MHz receiver

skycued needs neither decoder. `--no-readsb` (no connection to readsb) and
`--no-uat` (none to dump978-fa; `inputs.dump978.enable = false` does the same) leave out
the one you do not have:

| Receivers you have | skycued's inputs |
|--------------------|------------------|
| 1090 and 978 | readsb (1090 and UAT aircraft), dump978 (uplinks), plus the inputs file |
| 1090 only | `--no-uat` (or `inputs.dump978.enable = false`): readsb (1090 aircraft), plus the inputs file |
| 978 only | `--no-readsb`: dump978 (uplinks and FIS-B only), plus the inputs file |
| neither | `--no-readsb --no-uat`: the inputs file only |

With only a 978 MHz receiver skycued counts and decodes the uplinks but shows
no UAT aircraft: those reach it through readsb, which merges dump978-fa's
frames and needs a 1090 MHz receiver. A site whose only receiver is a
pingStation 3 (uAvionix's small networked ADS-B/UAT receiver) runs skycued with neither decoder and defines the pingStation
in [the inputs file](#the-inputs-file); the web UI or the API add and change
inputs while it runs. A built-in input that is left out is absent from the
inputs list.

With `recording.raw = true` skycued writes the raw frames of its pingStation
inputs to `recording.dir`; raw capture of a decoder's own output is
`skycue-rawrec`'s job ([Raw capture and replay](#raw-capture-and-replay)).

## Config keys that matter

The full list, with comments and the file grammar, is
`packaging/skycue.toml.example` (the packages install it as
`/etc/skycue/skycue.toml`; the Debian package generates that file from
`/usr/share/skycue/skycue.toml.example` when it is installed). The file is a
strict subset of TOML: tables such as `[receiver]` and `[inputs.readsb]`, and
keys of strings in quotes, numbers, `true`/`false` and arrays of strings. An
unknown table or key, a value of the wrong type or out of range, or a key
given twice stops skycued with exit status 2 and names the line. A key you
leave out keeps its default. An unexpected error
that escapes `skycued`, `skycue-rawrec` or `skycue-pingsim` ends it with
exit status 1 and one line, `fatal: <what>` at ERROR in skycued's log or
`skycue-rawrec: <what>` / `skycue-pingsim: <what>` on stderr.

Keys are written below as `table.key`. Every key except the four `filters`
keys is read once at startup: a change needs a restart (the filters apply at
once, see [Config tab](#config-tab)).

**`[receiver]`**

| Key | Meaning |
|-----|---------|
| `receiver.lat`, `receiver.lon` | receiver position in decimal degrees (required: skycued refuses to start while both are missing or both 0.0; `skycue-stats` does not need them) |
| `receiver.alt_m`, `receiver.antenna_height_m` | ground altitude above mean sea level and antenna height above the ground, in metres; their sum sets the elevation angles |

**`[inputs]`, `[inputs.readsb]`, `[inputs.dump978]`**

| Key | Meaning |
|-----|---------|
| `inputs.file` | the inputs file; empty (default): `skycue.inputs.json` beside the config, or in `paths.state_dir` when that is set |
| `inputs.decoder` | `"readsb"` (default) or `"skycued"`: who decodes the 1090 and 978 MHz frames of the built-in inputs, see [Decoding in skycued](#decoding-in-skycued); a change needs a restart |
| `inputs.readsb.enable` | `false`: there is no readsb on this machine, skycued does not connect to one (as `--no-readsb`, which applies last); default `true` |
| `inputs.readsb.host` | host of readsb (default `"127.0.0.1"`, not empty) (`--readsb-host` overrides it) |
| `inputs.readsb.json_port` | readsb's JSON port, `--net-json-port` (default 30047); read when `inputs.decoder = "readsb"` |
| `inputs.readsb.beast_port` | readsb's Beast port, `--net-bo-port`, 1 to 65535 (default 30005); read only under `inputs.decoder = "skycued"`; a change needs a restart |
| `inputs.dump978.enable` | `false`: there is no 978 MHz decoder, skycued neither connects to one nor expects UAT input (as `--no-uat`, which applies last); default `true` |
| `inputs.dump978.host` | host of dump978-fa (default `"127.0.0.1"`, not empty) (`--dump978-host` overrides it) |
| `inputs.dump978.raw_port` | dump978-fa's raw port, `--raw-port` (default 30978); where skycued reads UAT lines |
| `inputs.dump978.json_port` | dump978-fa's JSON port, `--json-port` (default 30979); skycued does not read it, the launcher passes it to dump978-fa |

**`[sdr]`**: read by the launcher that starts readsb and dump978-fa, not by
skycued, which only checks the values.

| Key | Meaning |
|-----|---------|
| `sdr.serial_1090`, `sdr.serial_978` | serial numbers as `rtl_test` prints them; empty: the only dongle, or let the program choose; the two must differ when both are set |
| `sdr.gain_1090`, `sdr.gain_978` | dB as a decimal in a string (`"49.6"`), or `"auto"`; the 1090 gain also takes `"auto-verbose"` (default `"auto"`) |

**`[cues]`**

| Key | Meaning |
|-----|---------|
| `cues.tcp_port` | cue clients connect here; `0`: no listener |
| `cues.tcp_connect` | `["host:port", ...]` of clients that listen; skycued dials them |
| `cues.udp_targets` | `["host:port", ...]` to send UDP cues to |
| `cues.min_interval_s`, `cues.max_interval_s` | per-aircraft cue rate: at most one per min (at least 0.1), at least one per max (not below min) |
| `cues.track_timeout_s` | silence before a track is dropped (at least 1) |
| `cues.keyframe_interval_s` | full-state keyframes on the stream (at least 1) |

The three forwarding modes (`cues.tcp_port`, `cues.tcp_connect`,
`cues.udp_targets`) can be used together; with all three off skycued runs but
warns that it forwards to nobody.

**`[filters]`**

| Key | Meaning |
|-----|---------|
| `filters.max_range_nm`, `filters.min_elevation_deg` | aircraft farther or lower are not sent |
| `filters.allow_list_file`, `filters.block_list_file` | curated lists, read at startup ([Curated lists](#curated-lists)); a relative path is taken from `paths.state_dir` |

**`[web]`**

| Key | Meaning |
|-----|---------|
| `web.port`, `web.bind` | the web UI ([The web UI](#the-web-ui)); `web.port = 0`: off; `web.bind = "0.0.0.0"` listens on every interface |
| `web.trail_seconds` | trail length on the web map, 0 to 3600 |

**`[recording]`**

| Key | Meaning |
|-----|---------|
| `recording.dir`, `recording.formats` | where and how to record (`["jsonl", "csv"]`, at least one while `dir` is set; see [Recordings](#recordings)) |
| `recording.raw` | write the pingStation JSON `rawData` frames to `recording.dir` (the decoders' raw streams are recorded by `skycue-rawrec`) |
| `recording.max_gb` | disk budget for the recordings (pass it to `skycue-rawrec --max-gb` too); `0`: unlimited |

**`[fisb]`**

| Key | Meaning |
|-----|---------|
| `fisb.enable`, `fisb.experimental` | decode FIS-B weather from the 978 MHz uplinks (default `true`); also decode the unverified layouts (default `false`). See [FIS-B weather](#fis-b-weather) |

**`[paths]`**

| Key | Meaning |
|-----|---------|
| `paths.state_dir` | absolute directory for skycued's files; a relative `inputs.file`, `paths.locations_file`, `stats.file`, `filters.allow_list_file` or `filters.block_list_file` is taken from it, and it holds the runtime overlay `skycue.runtime.json` and the `skycue.overrides.toml` that save as baseline writes (see [web-api.md](web-api.md#save-as-baseline)); empty (default): the places next to the config (next to `skycue.toml`; the list files from the working directory) |
| `paths.map_dir` | absolute directory the web map's `tiles/` and `overlays/` are served from, and nowhere else; empty (default): the web directory's |
| `paths.locations_file` | optional CSV `id,lat,lon` of report locations, merged under the learned ones; a relative path is taken from `skycue.toml`'s directory, or from `paths.state_dir` when it is set; read at startup (a change needs a restart). See [Report locations](#report-locations-fisb-locationsjson) |

**`[stats]`**

| Key | Meaning |
|-----|---------|
| `stats.file` | where the statistics history is kept (default `"skycue.stats.json"` in `paths.state_dir`, or next to `skycue.toml` when that is empty); a relative path is taken from `skycue.toml`'s directory, or from `paths.state_dir` when it is set; `""`: kept in memory only; a file that cannot be read back is renamed `<file>.bad`; read at startup (a change needs a restart). See [Statistics file](#statistics-file) |
| `stats.aircraft_max` | aircraft kept in the statistics ledger, a whole number 100..100000 (default 20000); past it the 5 % heard longest ago are forgotten; a change needs a restart |
| `stats.save_s` | seconds between saves of `stats.file` while it changed, a whole number 10..86400 (default 300); also saved at a clean shutdown and at once on a reset; a change needs a restart |

### Overriding a key for one run

Any key of `skycue.toml` can be changed for one run without editing the file:
`skycued --config skycue.toml --set inputs.readsb.host=10.0.0.2 --set web.port=0`
(repeatable, a later one wins), or `SKYCUE_INPUTS_READSB_HOST=10.0.0.2` in the
environment (the key upper-cased, `.` as `_`). The value is read as a TOML
value, except that a bare word is a string; an array needs its brackets,
`--set 'cues.udp_targets=["10.0.0.5:30500"]'`. Precedence, lowest first: the
built-in defaults, `skycue.toml`, `skycue.overrides.toml`, the environment,
`--set`, then the flags `--readsb-host`, `--dump978-host`, `--no-uat` and
`--no-readsb`. The startup log lists each key that does not come from the file
or the defaults with its source (`overrides`, `env`, `--set`, `flag`).
A variable whose first segment is a table (`SKYCUE_WEB_PROT`) but which matches
no key is logged once as a WARN at startup and ignored; variables whose first
segment is not a table are not skycued's and are ignored silently.

`skycued --config skycue.toml --dump-env` prints the effective value of every
key, all layers and flags applied, as shell assignments
(`SKYCUE_INPUTS_READSB_HOST='127.0.0.1'`; an array is its entries separated by
spaces). It is what the scripts read: `eval "$(skycued --config "$CONF"
--dump-env)"`. Do not export the variables: skycued reads `SKYCUE_*` from its
environment, so exporting them would pin every key to itself. On an error
nothing is printed on stdout and the exit status is 2.

### Converting an old skycue.conf

`skycued --convert-conf skycue.conf > skycue.toml` writes the TOML equivalent
of an old `skycue.conf` (the old `KEY=value` grammar, all its keys) on stdout.
The output is `packaging/skycue.toml.example` with your values filled in, so
its comments are the example's; the comments you wrote are not carried over,
and stderr says how many lines that was. A `skycue.overrides.conf` converts to
a `skycue.overrides.toml`. An unknown key or a malformed line exits with
status 2 and names the line. `--convert-conf` is a one-time aid and goes in a
later release.

### Where the files are

A `skycue.toml` copied from the example (the build-tree run) leaves
`paths.state_dir` and `paths.map_dir` empty, so skycued keeps its files next to
`skycue.toml`. The packaged config sets them, and an install from source sets
them by hand ([installing.md](installing.md#from-source)):

| File | Build tree (`paths.state_dir`, `paths.map_dir` empty) | Package (`paths.state_dir = "/var/lib/skycue"`, `paths.map_dir = "/var/lib/skycue/maps"`) |
|------|------|------|
| config | wherever `--config` points | `/etc/skycue/skycue.toml` (`/usr/local/etc/skycue/skycue.toml` from source); skycued never writes it |
| inputs, statistics, learned locations (`skycue.inputs.json`, `skycue.stats.json`, `fisb-locations.json`) | next to `skycue.toml` | in `paths.state_dir` |
| runtime overlay (`skycue.runtime.json`) | next to `skycue.toml` | in `paths.state_dir` |
| what **Save as baseline** writes | `skycue.overrides.toml` next to `skycue.toml`, and the list files | `skycue.overrides.toml` in `paths.state_dir`, and the list files |
| allow and block lists | as `filters.allow_list_file` / `filters.block_list_file` say (relative: skycued's working directory) | `allow.list`, `block.list` in `paths.state_dir` |
| map tiles and overlays | `tiles`, `overlays` in the `--www-dir` directory | `paths.map_dir/tiles`, `paths.map_dir/overlays` |
| recordings | `recording.dir` | `recording.dir` (commented out; `/var/lib/skycue/recordings` is ready for it) |
| log | standard error of the terminal it runs in | the journal (`journalctl -u skycued`) |

`skycue.overrides.toml` is in the same TOML grammar and holds only
the keys the page may save (`filters.max_range_nm`, `filters.min_elevation_deg`,
`filters.allow_list_file`, `filters.block_list_file`); skycued reads it after
`skycue.toml`, so its values win. skycued owns the file: it writes it whole,
with a header line saying so, so comments you add are lost the next time the
page saves. Hand edits are picked up while skycued runs. The packaged
`skycue.toml` is a file the package manager owns, so the daemon, which runs as
the `skycue` user with only `/var/lib/skycue` writable, never writes it. To
make a saved value part of `skycue.toml` again, copy it there and delete the
line from `skycue.overrides.toml`.

### Connection logging

skycued dials the decoders (readsb, dump978), any pingStation input and the
cue targets, and listens for the rest, through one network library. What it
logs about a connection:

- **A connect that fails** logs one WARN per outage, then DEBUG for the
  following attempts until a connection succeeds:
  `<input>: connect <host>:<port>: <address>: <why>; retrying in <N> ms
  (further failures logged at debug level)`. A failed name lookup reads
  `resolve <host>: <why>` in the same place. This format is the same for every
  dialled connection (inputs and cue targets); earlier releases worded each
  one a little differently.
- **A connection that was up and is lost** is logged by skycued itself, at
  INFO (`<input>: connection closed (<why>)`; for a cue target or client,
  `forwarder: TCP target|client <peer> disconnected (N left)`, or the error
  text when the connection was reset, or a WARN when a client was cut off for
  falling more than the queue limit behind, or INFO `closed for shutdown` for
  each one at stop). The network library's own line for the loss is DEBUG. The
  first failed attempt after the loss is the WARN above.
- **A connect gives up on an address after 10 s.** Earlier releases waited as
  long as the kernel did (typically about two minutes) for readsb, dump978,
  pingStation and cue dials; a decoder behind a very slow link may now need
  the connection retried.
- **A listening input whose port cannot be bound** (a `tcp_listen` pingStation
  input) logs a WARN the first time the bind fails and DEBUG for the retries.
  Earlier releases logged nothing for it (the reason showed only in the
  input's status); the WARN is intended.
- **Accept failures and UDP send failures** are logged at DEBUG by the
  library; the cue UDP targets that cannot be set up at start are an ERROR
  (`forwarder: UDP target <host:port> skipped: <why>`), and a frame too large
  for a datagram is an ERROR (`forwarder: frame of N bytes too large for UDP,
  not sent`).
- **Tools.** `skycue-pingsim` and `skycue-rawplay` dial with TCP keepalive
  and with `TCP_NODELAY` off (unlike skycued). Their reconnect lines carry the
  reason: `connect <host>:<port>: <address>: <why>; retrying in <N> ms`.
  `skycue-pingsim` serves one client at a time.

## Decoding in skycued

By default readsb decodes the 1090 MHz frames and skycued reads its JSON
(`inputs.decoder = "readsb"`). With

```ini
inputs.decoder = "skycued"
inputs.readsb.beast_port = 30005
```

skycued decodes the Mode S frames itself. The built-in `readsb` input stops
reading the JSON port and becomes a `beast_raw` input that dials
`127.0.0.1:inputs.readsb.beast_port` (`inputs.readsb.host` and `inputs.readsb.json_port` are not
used for it), so the decoder you run must serve Beast output on that port;
the track table, filters, cues, recordings and web page work as before. The
decoder also runs on any other `beast_raw` input you add, and with it on
every such input produces observations. What changes:

- **Position reference.** A `beast_raw` input may carry `ref_lat` and
  `ref_lon` (both or neither; a `beast_raw` or `uat_raw` input only) for its
  decoder's range check; without them it uses `receiver.lat` and
  `receiver.lon`. Setting them makes the inputs file version 2.
- **UAT is decoded too.** The built-in `dump978` input and every `uat_raw`
  input decode the 978 MHz downlinks (see [UAT (978 MHz)](#uat-978-mhz-in-skycued));
  FIS-B uplinks and their counts are unchanged. The `uat_in` connector in
  readsb is not needed and should be dropped: a readsb JSON input that still
  carries the UAT traffic double-feeds it, and `--check-inputs` and the
  startup log warn about that as they do for 1090.
- **The `json` output carries nothing.** It is built from readsb's JSON
  input; with that input gone it stays defined and enabled but idle.
- **Leftover readsb inputs double-feed.** An enabled input of type `readsb`
  (JSON) next to an enabled `beast_raw` input counts every aircraft twice.
  skycued logs a WARN once per change and `--check-inputs` prints it:

  ```
  WARN: inputs.decoder=skycued: input 'ps-extra' (readsb JSON) and input 'readsb' (beast_raw) both feed the track table, so aircraft are counted twice; disable one
  WARN: inputs.decoder=skycued: the json output 'json' carries nothing, since no readsb JSON input is enabled; it stays defined and enabled
  ```

  Disable one of the two inputs. Under `skycued`, `--check-inputs` probes
  `127.0.0.1:inputs.readsb.beast_port` and prints it as the `readsb` line.
  Under `readsb` nothing changes.
- **Counters.** A decoding input shows the decoder's counters in
  [`GET /api/inputs`](web-api.md#inputs), and `GET /api/stats` gets a
  `decoder` object ([the stats object](web-api.md#the-state-object)).
- **Replay.** Play a capture to the decoder with
  `skycue-rawplay --1090-listen PORT` and point `inputs.readsb.beast_port` (or a
  `beast_raw` input) at it; for UAT, `skycue-rawplay --uat-listen PORT` and a
  `uat_raw` input pointed at that port; the simpler route is to set
  `inputs.dump978.raw_port` to it, so the built-in `dump978` input reads the replay. See [Raw capture and replay](#raw-capture-and-replay).

The default keeps readsb's own decoding and its JSON. The decoder follows the
public Mode S layouts and was compared with readsb on the same capture;
Comm-B vertical rates (BDS 6,0) are not decoded yet, so the barometric
vertical rate of a few aircraft can differ from readsb's in interrogated
airspace (the comparison accepts 95 % agreement on that one value, and measured
about 97 % on the 120 s capture).

### UAT (978 MHz) in skycued

Under `inputs.decoder = "skycued"` the built-in `dump978` input (dump978-fa's raw
port, `inputs.dump978.host` and `inputs.dump978.raw_port`) and every `uat_raw` input decode the downlink lines
(`-HEX;...`) themselves; no setting beyond `inputs.decoder` is needed.
Uplink lines (`+HEX;...`) go to the FIS-B decoder as before. A downlink of
18 bytes (payload type 0) or 34 bytes (types 1 to 10) is decoded; anything
else that is hex counts in `rejected`. What you get:

- **Fields.** Address, address qualifier, position, barometric or geometric
  altitude, ground speed and track (ground or air), vertical rate and its
  source, NIC, NACp, emitter category, emergency status, call sign or squawk,
  and the air/ground state. Long frames also carry the second altitude, so
  both barometric and geometric are known and their difference is reported.
- **Address qualifiers.** 0 and 1 (ADS-B), 2 (TIS-B or ADS-R with an ICAO
  address; skycued does not tell the two apart), 3 (TIS-B track file), 4
  (surface vehicle), 5 (fixed beacon) and 6 (ADS-R with a non-ICAO address)
  are mapped to the address type and source of the track; 7 is reserved and
  counted in `unknown_type`, as is a frame whose length does not fit its
  payload type.
- **Categories and emergencies.** Emitter categories 32 to 39 are shown as
  `E0` to `E7`, the way dump978-fa prints them. Emergency code 3 is shown as
  `minimum_fuel` (the public table); dump978-fa prints `invalid` for it.
- **Position range.** A position more than 300 NM from the input's
  reference (`ref_lat` and `ref_lon`, else `receiver.lat` and
  `receiver.lon`) is rejected and counted in `cpr_rejected`. There is no
  speed check against the previous fix (UAT positions are whole fixes, and the
  error correction makes a corrupt one very rare). A change without a position (an altitude, say)
  is reported only after 10 s without an accepted position, and at most once a
  second. Call sign, category, squawk and emergency status are kept for as long
  as the aircraft is tracked, on both bands; all other fields for 60 s.
- **Counters.** The same set as for 1090 ([`GET /api/inputs`](web-api.md#inputs)),
  and the `decoder` object of `GET /api/stats` sums them. The Feeds tab shows
  one line on the card of a decoding input: frames per second, the share of
  frames that failed the CRC, and the counts of frames the decoder repaired
  (`fixed`), positions waiting for their pair (`pending`), positions rejected
  (`rejected`) and frames of a type it does not decode (`unknown`). dump978-fa
  has already done the error correction, so `crc_fail` and `crc_fixed` stay 0
  for UAT; `rs_errors` sums the Reed-Solomon symbol errors dump978-fa reports
  per frame (`rs=`); `unmapped` counts frames with a position of payload
  type 7 to 10, whose state vector is decoded and reported but whose extra
  content is not (without a position they count in `no_position`). Target State
  content of types 3, 4 and 6 is not decoded either, but those frames count in
  `decoded`.
- **Geometric altitude.** A UAT long frame reports both altitudes, so the
  difference is the aircraft's own and feeds the population fit used for
  estimates ([below](#geometric-altitude)). TIS-B track-file targets (qualifier
  3) carry no geometric altitude and get an estimate like 1090 aircraft without
  one.

**How it was validated.** The decoder was compared field by field with a
dump978-fa build on three public captures (the dump978 sample data and two
Stratux logs, 439, 3,760 and 303 downlinks) and on a synthetic matrix of 109
frames built to carry all 40 emitter categories, all 8 emergency codes, the
four ground track types, all NIC and NACp values and representative rows for
the rest (qualifiers, velocity, vertical rate, altitude and position limits,
squawk and call sign, AUX). All are payload type 1 long frames. The
golden files are in `tests/fixtures/raw/` and the tests run without any
other decoder installed. A live differential plays the captures into skycued
as a `uat_raw` input and compares the tracks it builds with dump978-fa's
output per aircraft: every compared field agreed on the 22, 43 and 7
aircraft compared (positions, altitudes, speed, track, vertical rate,
identity, NACp, category). Identity and geometric altitude are compared only
where both sides report them, and one aircraft in the FlightAware sample lies
outside the 300 NM limit and is excluded. To regenerate the goldens or run the
differential against your own build, set `SKYCUE_DUMP978` to a dump978-fa
executable (`SKYCUE_READSB` does the same for a readsb in the 1090
tests); without them the committed goldens are used.

**What is not verified against real data.** The captures hold no
emitter category above A2, no emergency code other than none, no air/ground
state 3, no use of the spare bit 98, no reserved call-sign or squawk symbols
(38, 39) and no payload type above 2. The real captures cover payload types 0,
1 and 2 (type 2 frames: 78 in the FlightAware sample, 42 in the Stratux
capture, all agreeing with dump978-fa); the synthetic matrix covers type 1;
types 3 to 10 (mode status without AUX, the length and type pairing of 4 and
7 to 10) are not verified against dump978-fa and rest on the public layout and
unit tests. The categories and emergency codes rest on the public layout and
the synthetic matrix read by dump978-fa, not on traffic. Emitter categories 32 to 39 follow
dump978-fa alone, since the public table has no set E. Treat a track with one
of these values as unconfirmed, and report a frame that disagrees with
dump978-fa.

### pingStation decode mode

A `pingstation_json` input normally trusts the fields the device decoded.
With `"source":"decode"` skycued ignores those fields and decodes the
`rawData` frame of each aircraft itself, 1090 or UAT by length (7 or 14
bytes: Mode S; 18 or 34 bytes: UAT), whatever `inputs.decoder` is. The
position reference is the device's own status position when it has polled
one (`status_url`), else `receiver.lat` and `receiver.lon`.

```json
{"id":"ps-north","type":"pingstation_json","port":4000,"source":"decode"}
```

`source` is `device` (the default) or `decode`, applies to
`pingstation_json` only, and `decode` makes the inputs file version 2. Notes:

- A device repeats its snapshot every few tenths of a second, so a frame
  byte-equal to one already decoded less than 1 s earlier is skipped and
  counted in `raw_duplicates`; a repeat is decoded again once a second has
  passed since the last decoded copy.
- An aircraft without `rawData` cannot be decoded and counts in `no_raw`;
  it does not reach the track table. Check that the device sends raw frames
  before switching a receiver to `decode`.
- The decoder counters of [UAT](#uat-978-mhz-in-skycued) and Mode S appear on the
  input, and the Feeds tab offers `source` in the input form and the decoder
  line on the card.

## Geometric altitude

Receivers differ in what they send: many aircraft report a barometric
altitude all the time and a geometric (GNSS) altitude rarely or never. For
the elevation filter, the cues and the page, skycued fills the gap with an
estimate, under every `inputs.decoder`.

- **Reported.** A geometric altitude the aircraft sent less than 60 s ago, and
  no newer barometric altitude by more than 2 s since. The track carries
  `"alt_geom_source":"reported"` and is exactly what it was before.
- **Estimated.** Otherwise, with a barometric altitude under 10 s old and the
  aircraft not on the ground:
  1. if the aircraft's own difference between geometric and barometric
     altitude (from a TC 19 velocity message, from a TC 20-22 height paired
     with a barometric altitude, or from a reported `alt_geom`) is at most
     300 s old, estimate the barometric altitude plus that difference,
     corrected by a slope for the altitude change since;
  2. else, with a fit over other aircraft (below), the barometric altitude
     plus the fit's difference at that altitude;
  3. else a reported geometric altitude up to 60 s old stays as reported;
  4. else no estimate: the track simply has no geometric altitude.
  The fit is made of the differences aircraft reported in the last 120 s: a
  straight line in barometric altitude when at least 5 distinct aircraft and
  20 samples are in the window, else, with at least 3 aircraft, their median
  (with fewer than 3 there is no fit). It is visible as
  `altitude_fit` in `GET /api/stats`.
- **No made-up numbers.** There is no estimate without a barometric altitude
  under 10 s old, none on the ground, and none when there is neither an own
  difference nor any other aircraft's difference in the window. With fewer than
  5 aircraft or 20 samples in the window the fit is the median of the
  differences (needs 3 aircraft), not a line, and its stated uncertainty is
  large (at least 600 ft); read
  `altitude_fit.aircraft` in `GET /api/stats` for how many it rests on.
- **Where it shows.** Tracks carry `alt_geom_source`, `alt_geom_uncertainty_ft`
  (estimates only), `alt_geom_age_s` and `geom_minus_baro_ft`
  ([web-api.md](web-api.md#the-state-object)). The page's aircraft box reads
  `12,300 ft est. ±250` for an estimate and has rows for the source,
  uncertainty, age and the difference. Cue clients see an estimate as an
  ordinary `alt_geom_ft` plus the uncertainty ([protocol.md](protocol.md#trackupdate-type-2)).
  Recordings keep what the input said: the received stream never holds an
  estimate.
- **How good it is** (the `geom_eval` test, which prints these numbers).
  Against other aircraft on the 120 s capture (244 aircraft, 5,239
  samples), the error of a 60 s or 120 s fit is about 165 to 175 ft at the 90th
  percentile (about 75 ft median, at most about 700 ft); a plain median of
  the others is about 550 to 575 ft. Held from the aircraft's own difference
  (five recorded flights, about 35,000 samples), the 90th percentile is about
  52 ft after 60 s and about 185 ft after 300 s; rare maxima reach about
  2,000 ft in a climb or descent. The uncertainty on the wire is stated
  conservatively: 75 ft for an own difference under 60 s old, growing from
  125 ft at 60 s to 325 ft at 300 s, and for a fit its 90th percentile rounded
  up to 25 ft (at least 100 ft for a line, at least 600 ft for the median). Treat an estimate as good to a few hundred
  feet, not a measurement.

## Replaying a JSONL recording

```sh
skycued --config skycue.toml --replay received-20261101-120000.jsonl                      # real time
skycued --config skycue.toml --replay received-20261101-120000.jsonl --replay-speed 10    # 10x
skycued --config skycue.toml --replay FILE --replay-speed 0 --replay-loop                 # unpaced, over and over
```

`--replay FILE` runs skycued with the recording as its only input: no
readsb, no dump978-fa, no SDRs. FILE is a received-stream recording written
by skycued, or a capture of readsb's JSON port (one JSON object per line).
Everything downstream is as in a live run: the same filters, the same
forwarding, and recording too if `recording.dir` is set. `--replay-speed 1`
(the default) keeps the recorded timing; `--replay-speed 0` sends as fast as
skycued can; `--replay-loop` starts the file again at its end. Only the
receiver position is needed from the config.

A replay opens the same ports as a live skycued, so it cannot run beside one
that is using them. Stop the service first (`sudo systemctl stop skycued`),
or give the replay its own ports: `--web-port 8081 --cue-tcp-port 0` (or
`web.port` / `cues.tcp_port` in a copy of the config), and point `paths.state_dir`
and `recording.dir` of that copy elsewhere so the replay does not mix into the
live files. From a source tree, add `--www-dir www`.

## Raw capture and replay

`skycue-rawrec` records what the decoders produce before any
interpretation: readsb's Beast output as `beast-*.bin` and dump978-fa's raw
UAT lines as `uat-*.txt`, a new file every 256 MB or 60 minutes (`--rotate-mb`,
`--rotate-min`). These let a session be decoded again later, by a newer
readsb for instance. It is a separate process from skycued; run it yourself
or from a unit of your own:

```sh
skycue-rawrec --beast 127.0.0.1:30005 --uat 127.0.0.1:30978 --dir /var/lib/skycue/recordings/raw
```

`--max-gb N` keeps the recordings within N GB by deleting the oldest (with
`--retention-dir` naming the directory that holds `raw/` when `--dir` is not
`<recording.dir>/raw`). Leave out `--beast` or `--uat` for a receiver you do not
have. `recording.dir/raw` is where [Recordings](#recordings) expects them.

Each of `--beast` and `--uat` takes `HOST:PORT` (or the same as
`tcp://HOST:PORT`), which connects and reconnects with a backoff from 0.5 s to
10 s, or `udp://BIND:PORT`, which binds that address and port and records the
datagrams that arrive (for UAT, the lines in them).
UAT lines longer than 64 KiB are dropped whole (they used to be truncated),
and every `\r` in a line is stripped, not only a trailing one.

`skycue-rawplay` plays them back into a network-only readsb, so the decoder
interprets them again:

```sh
readsb --net-only --net-ri-port 30001 --net-bi-port 30004 --net-json-port 30047 \
       --lat 52.0 --lon 4.5 --net-connector 127.0.0.1,30978,uat_in
skycued --config skycue.toml          # reads readsb's JSON port as it would live
skycue-rawplay --beast-in 127.0.0.1:30004 --raw-in 127.0.0.1:30001 --uat-listen 30978 \
       --speed 1 beast-20261101-120000.bin uat-20261101-120000.txt
```

Accepted files: `.bin` (Beast, to `--beast-in`), `.csv[.gz]`
(`epoch_seconds,HEX` Mode S lines, to `--raw-in`) and `.txt[.gz]`/`.log`
(dump978 raw UAT lines, served on `--uat-listen`, which readsb connects to
with the `uat_in` connector above). `--speed N` (0: as fast as possible) and
`--loop` work as for a JSONL replay. Since rawplay serves the UAT lines to
readsb only, skycued does not see the UAT port and uplinks are not counted.
To track the UAT lines in skycued instead, serve them to a `uat_raw` input
(`inputs.decoder = "skycued"`; no readsb is needed for them):
`skycue-rawplay --uat-listen 30978 uat-20261101-120000.txt`.
The receiver position given to readsb (and in `skycue.toml`) must match the
capture's region, or readsb cannot decode positions and the range filter
drops everything: the sample capture in `tests/fixtures/raw/` is European,
so use `52.0` / `4.5` for it.

`--1090-listen PORT` serves the Beast frames on 127.0.0.1 instead of dialing
readsb: `.bin` files as recorded, `.csv` files as type 1, 2 or 3 frames (the
twelve-hex-digit timestamp of the `@` lines, signal byte 0). `.bin` files go
to `--beast-in` or to `--1090-listen`, `.csv` files to `--raw-in` or to
`--1090-listen`, and one listening port serves one kind. Both listening ports
(`--1090-listen` and `--uat-listen`) serve any number of clients at once,
each of which gets every frame or line: readsb (`--net-connector
127.0.0.1,PORT,beast_in`) and skycued's `beast_raw` input can read the same
replay at the same time. `--wait-clients N` (default 1) holds playback of each
listening port in use until N clients are connected to it; if every client
leaves later, playback waits for one again. A client that stops reading costs
only itself: when 1 MiB is queued for it, further data for that client is
dropped, and the number of bytes dropped is printed at the end
(`... 0 reconnects, 0 bytes dropped (slow clients)`). With `--speed 0` nothing
is dropped: playback waits for the slowest client, so at `--speed 0` a client
that connects and never reads stalls playback, and the exit, for good.
What is dropped is always whole frames or whole lines: rawplay only hands the
listener complete Beast frames and complete UAT lines, so a client never sees
a record cut in the middle.

## IQ files

`skycue-iqgen` makes 8-bit unsigned I/Q (`cu8`) files from recordings, and a
script fetches a public off-air capture; the decoders themselves can then be
run on them, readsb with `--device-type ifile --ifile FILE --iformat UC8` and
dump978-fa with `--file FILE --format CU8`, in place of the SDRs. As with
raw replay, the receiver position must match the capture: `52.0` / `4.5`
for files made from the sample Mode S capture, `41.15` / `-8.61` for the
Porto capture. Making the files, the real capture and what the decoders
recovered from both are described in [iq-replay.md](iq-replay.md).

## The inputs file

skycued's inputs are the two built-ins, `readsb` and `dump978` (the decoders
skycued connects to; a built-in you run without is
absent, see [Running without a 1090 or 978 MHz receiver](#running-without-a-1090-or-978-mhz-receiver)), plus any inputs you add, such as a pingStation 3.
The added inputs and the enabled state of the built-ins are kept in
`skycue.inputs.json` in `paths.state_dir` when that is set (`/var/lib/skycue` in
the packages), else next to `skycue.toml`. It is the only place they are
stored: a change made in the web UI or the API is written there at once
(with an atomic rename), and there is nothing to "save" separately. Until the
first change there is no file, and skycued runs the built-ins only. The
API's `/api/inputs` routes make the same changes and report each input's
state and counters; they and every definition key are described in
[web-api.md](web-api.md#inputs). The same file holds the [outputs](#outputs).

```json
{"version":1,
 "inputs":[
  {"id":"ps-north","name":"North mast","type":"pingstation_cat021","enabled":true,"transport":"udp","bind":"0.0.0.0","port":4001,"peer":null,"edition":"2.5","sac":null,"sic":null,"status_url":null,"status_interval_s":30,"time_mode":"utc","capture":false}
 ],
 "builtin":{"readsb":{"enabled":true},"dump978":{"enabled":true}}}
```

`version` is 1 or 2. A version 1 file loads as it always did, and skycued
writes version 1 back for as long as the file holds nothing from version 2:
an `outputs` array, an input of type `beast_raw` or `uat_raw`, a `group`,
`interface` or `format` key, a `transport` key on a `readsb`, `dump978` or
`pingstation_json` input (version 1 allowed it on `pingstation_cat021` only),
or a cue output's id in `builtin`. A version 1 header with any of these is
refused (`'outputs' needs inputs file version 2`), and so is a version above
2. Adding the first such entry through the page or the API turns the file into
version 2; removing the last one turns it back into version 1.
`inputs`, `outputs` and `builtin` may be left out; a built-in
missing from `builtin` keeps its default (enabled). Each entry follows the
definition rules of the API (an unknown key, a key that does not apply to
the type, or a value out of range is an error), and two enabled listeners,
inputs or outputs, may not use the same port and protocol, except that
several multicast receivers may share a port. A listener may not take the
port of the cue stream (`cues.tcp_port`) or of the web server (`web.port`). `"capture":true` needs `recording.dir`:
it writes every datagram (or TCP block) the input receives to
`recording.dir/capture-<id>-*.jsonl`, one JSON line each. Without `recording.dir`
the web UI and the API refuse it (400 "capture needs recording.dir"); in the
file, at startup or in a hand edit, the entry is kept and runs, skycued logs
one WARN ("capture needs recording.dir: not capturing") and nothing is captured. With `recording.raw = true` a `pingstation_json` input also writes the raw 1090 and
UAT frames the device includes (`rawData`) to `recording.dir/raw-<id>-*.csv` and
`.txt`, which `skycue-rawplay` accepts like skycue-rawrec's
files. A `pingstation_json` input may also decode those frames itself ([pingStation decode mode](#pingstation-decode-mode)). Both formats are in [recording-format.md](recording-format.md#pingstation-capture-and-raw-frames).

**Hand edits.** skycued checks the file about once a second and applies a
change once the file has stopped changing for a second, so an editor that
saves in several steps is never read half written. The change is applied by
id: a new id is started, a removed one stopped, a changed definition restarts
that input (its counters carry on), and inputs you did not touch keep
running undisturbed.

**A file that is not valid** (bad JSON, a bad entry, a port clash; not a
`"capture":true` without `recording.dir`, which is only warned about) is
renamed `skycue.inputs.json.rejected` and the reason is logged at ERROR. At
startup skycued then runs the built-ins only; while running it keeps the
inputs it had. Fix the file and put it back under its own name: skycued keeps
watching that name and loads the new file. Deleting the file while skycued
runs stops nothing. The next change from the UI or API writes a fresh file
from the running set and leaves the `.rejected` copy alone. Only the latest rejected file is kept: a
second rejection replaces the earlier `.rejected` copy.

The file is read through Boost.PropertyTree, which keeps every value as
text. Three consequences:

- `"inputs":{}` reads as no inputs, the same as `"inputs":[]`.
- A quoted number or boolean is accepted as the value: `"port":"4001"` is
  port 4001 and `"enabled":"true"` is true.
- A JSON `null` and the string `"null"` both read as unset: `"peer":null`
  and `"peer":"null"` both accept any peer.

skycued writes plain JSON values, so this matters only for a file edited by
hand. A replay (`skycued --replay`) neither reads nor changes the file.

### Raw inputs and endpoint keys

Every input type takes the endpoint keys of its transport:

| Key | Meaning |
|-----|---------|
| `transport` | `tcp_connect` (dial `host`:`port`), `tcp_listen` (accept on `bind`:`port`) or `udp` (receive on `bind`:`port`) |
| `host`, `port` | the target of `tcp_connect` |
| `bind`, `port`, `peer` | the local address (default `0.0.0.0`) and port of `tcp_listen` and `udp`; `peer` accepts only that source address |
| `group`, `interface` | `udp` only: join the multicast group `group` (224.0.0.0 to 239.255.255.255), on the local interface address `interface` (the system's choice when left out); `bind` does not apply with a group |

Which transports a type takes: `readsb` and `dump978` any (the built-ins stay
`tcp_connect` to `inputs.readsb.host` and `inputs.dump978.host`; a second `readsb` or
`dump978` input in the file may listen or receive instead), `pingstation_json`
any (`udp` by default, as before), `pingstation_cat021` any (required),
`beast_raw` and `uat_raw` any (`tcp_connect` by default). A UDP datagram of a
line-based type (readsb, dump978 or `uat_raw`, `pingstation_json`) holds one
or more lines; the last may lack its newline.

Two types read the decoders' raw output directly:

- **`beast_raw`** takes Beast binary frames from a decoder's Beast port
  (30005 on a readsb, or `skycue-rawplay --1090-listen`). The frames are cut
  by the same framer the raw tools use, per TCP connection and, for UDP, per
  sending address, so a frame may span datagrams and a datagram may hold
  several. `"format":"avr"` takes AVR text lines (`*8D...;` or `@...;`)
  instead and turns each into a type 2 or 3 Beast frame with a zero
  timestamp and signal byte 0; everything downstream, and the capture, sees
  Beast frames. Skycue does not decode them: the input counts `messages`
  (frames), `bytes` and `rejected` (bytes the framer skipped, lines that were
  not Mode S), and the frames go to the `beast` outputs.
- **`uat_raw`** takes dump978's raw lines. It is the general form of the
  built-in `dump978`: downlinks are counted and uplinks reach the FIS-B
  decoder exactly as the built-in's do, a second copy of an uplink another
  input already delivered is dropped as a duplicate, and the lines go to the
  `uat` outputs. Under `inputs.decoder = "skycued"` the downlinks are decoded to
  aircraft too ([UAT in skycued](#uat-978-mhz-in-skycued)). The built-in keeps its id `dump978`, and `dump978` stays a
  valid `type` for an input you add (it is the same input class).

`"capture":true` on a `beast_raw` or `uat_raw` input needs `recording.dir` like
the pingStation capture (the same warning, the same 400 from the API). It
writes what the input published, in the raw tools' formats, so
`skycue-rawplay` plays it back: `raw-<id>-YYYYMMDD-HHMMSS.bin` for Beast
(the bytes of the frames in arrival order, also for an `avr` input) and
`raw-<id>-YYYYMMDD-HHMMSS.txt` for UAT (`<epoch_ms> <line>`). They rotate and
count against the retention limit like the other raw files
([recording-format.md](recording-format.md#raw-capture-of-beast_raw-and-uat_raw-inputs)).

`skycued --check-inputs` dials every enabled `tcp_connect` input of the file,
`beast_raw` and `uat_raw` included, and reports the other inputs as not
checked; the file's outputs are validated with the inputs (a port or
multicast clash is an error, and a loopback output to a port this daemon
listens on is a `WARN:` line before the check lines).

## Outputs

skycued is also a hub: each input hands the messages it receives, before any
parsing and in arrival order, to the outputs, which serve them to other
programs. An output is a TCP listener, a TCP target skycued dials, or a UDP
sender (unicast or multicast), set up in the page, through
[the API](web-api.md#outputs) or in the `outputs` array of the inputs file:

```json
{"version":2,
 "inputs":[
  {"id":"lab","type":"beast_raw","transport":"tcp_connect","host":"127.0.0.1","port":30005}
 ],
 "outputs":[
  {"id":"beast-out","name":"Beast to the lab","kind":"beast","transport":"tcp_listen","bind":"0.0.0.0","port":30105},
  {"id":"beast-mc","kind":"beast","transport":"udp","group":"239.255.10.5","port":4978,"interface":"192.0.2.10","ttl":1},
  {"id":"cue-ops","kind":"cue","transport":"tcp_connect","host":"192.0.2.30","port":30500}
 ]}
```

Keys: `id` (1 to 32 of `a-z`, `0-9`, `-`), `name`, `kind`, `enabled` (default
true), `transport` (required: `tcp_listen`, `tcp_connect` or `udp`), then by
transport `bind`, `port` and `peer` (`tcp_listen`; `peer` accepts only that
address), `host` and `port` (`tcp_connect`; `udp` to a unicast `host`), or
`group`, `port`, `interface`, `ttl` and `loopback` (`udp` to a multicast
group: `interface` is the local interface address, `ttl` 1 to 255 defaults to
1, `loopback` defaults to true so a consumer on the same machine hears it).
`host` and `group` are exclusive, and `ttl`, `interface` and `loopback` need
a `group`.

| `kind` | One message | Over TCP | Per UDP datagram |
|--------|-------------|----------|------------------|
| `beast` | a Beast frame as received, escapes and timestamp untouched | the byte stream | one frame |
| `uat` | a dump978 raw line | lines | one line |
| `json` | a readsb JSON line | lines | one line; a line over 65507 bytes is dropped and counted |
| `pingstation_json` | a pingStation JSON text | one line each; a text that itself contains a newline is dropped and counted | one text, as received |
| `cat021` | a CAT021 block as received | the stream (blocks delimit themselves) | one block |
| `cue` | the skycue protocol frames ([protocol.md](protocol.md)) | as `cues.tcp_port` | as `cues.udp_targets` |

An output carries the messages of every enabled input of its kind, merged in
arrival order: a `beast` output serves the frames of all `beast_raw` inputs.
Where an input's data arrives from a decoder skycued already reads for
another purpose, nothing is lost: the `json` output serves what the built-in
`readsb` input reads, the `uat` output what `dump978` reads, and so on. An
output with no input of its kind is valid; a listener that has nothing to
serve yet. (When the 1090 decoder moves into skycued, `inputs.decoder = "skycued"`,
there is no readsb JSON to republish and the `json` output carries nothing.)

An output is started and stopped like an input: `enabled` turns it off and
on, and an edit restarts it; a listener or sender that cannot bind retries
every 5 s and shows the reason as its `last_error`. A TCP consumer that falls
more than 1 MiB behind is disconnected (counted in `dropped`) and may
reconnect; the others are not delayed by it. At most 16 consumers are
served at once on a TCP listener. UDP is best effort: a datagram the kernel
cannot take is dropped and counted. Two safeguards in validation: an output
that sends to a multicast group and port one of this daemon's inputs joins
is refused as a loop, and one that sends to a loopback address and a port
this daemon listens on gets a `WARN:` in the log.

Each output keeps counters for its lifetime, across edits: `peers`
(connected consumers), `bytes` and `messages` sent (a TCP message sent to
three consumers counts three times), `dropped`, and `last_error`. The page
and `GET /api/outputs` show them.

**The cue outputs.** What `cues.tcp_port`, `cues.tcp_connect` and
`cues.udp_targets` configure shows up as built-in outputs of kind `cue` with the
ids `cue-tcp`, `cue-connect-N` and `cue-udp-N` (N counts from 1 in the order
of the config value), flagged `"builtin":true`. They cannot be edited or
removed, but can be disabled and enabled, like the built-in inputs; the
choice is kept in the file's `builtin` object (`"builtin":{"cue-tcp":{"enabled":false}}`,
which makes the file version 2). Disabling closes the connections and sends
no drops. The config keys are unchanged and there are no new ones; outputs
you add are `cue` outputs too, with their own ids, and serve the same
protocol, Hello and keyframe burst to each new consumer. The Cues tab lists
the sessions of all of them, and `/api/cues/clients` names the output of each.

**Upgrade note: input names.** A `name` must be valid UTF-8 of at most 64
characters with no control character. Earlier versions refused only C0
controls (U+0000 to U+001F) and DEL; skycued now also refuses the C1
controls (U+0080 to U+009F) and invalid UTF-8 (a stray or missing
continuation byte, an overlong form, a surrogate). The API refuses such a
name with `400`, as before for C0 controls. A name already stored in the
inputs file is not fatal: at load skycued repairs it (each invalid byte
becomes U+FFFD, control characters are dropped, the name is cut to 64
characters), logs `input '<id>': name repaired` at ERROR, and loads every
input; the repaired name is written back on the next change. To see such
names before upgrading:

```sh
python3 -I -c 'import json,re,sys; d=json.loads(open(sys.argv[1],"rb").read().decode("utf-8","surrogateescape")); [print(i.get("id"), ascii(i.get("name"))) for i in d.get("inputs", []) if re.search("[\udc80-\udcff\x80-\x9f]", str(i.get("name") or "")) or len(str(i.get("name") or "")) > 64]' etc/skycue/skycue.inputs.json
```

It prints the id and name of each input whose name is not valid UTF-8,
holds a C1 control or is over 64 characters; nothing when all are fine.

## A pingStation without hardware: skycue-pingsim

`skycue-pingsim` plays the part of a pingStation 3 towards a skycued input.
It is how the Feeds tab is demonstrated and how the end-to-end test runs.
It reads one of two kinds of file:

- `--capture FILE`: a capture file written by an input with
  `"capture": true` (`capture-<id>-*.jsonl`, see
  [recording-format.md](recording-format.md#pingstation-capture-and-raw-frames)).
  Every record's bytes go out exactly as they were received, paced by the
  record's `t`.
- `--as json FILE` or `--as cat021 FILE`: a received-stream recording
  (`received-*.jsonl`) or a capture of readsb's JSON port, such as
  `tests/fixtures/readsb-sample.jsonl`. Each observation is
  synthesised as a pingStation JSON aircraft object, or as a one-record
  CAT021 edition 2.5 block, stamped with the time it is sent and paced by the
  observation times. Sent-stream records and uplinks are skipped.

FILE may be gzip-compressed. pingsim sends to one of:

- `--udp HOST:PORT`: one datagram per record, for a `pingstation_json` input
  or a `udp` `pingstation_cat021` input. HOST can be a multicast group
  (`--udp 239.1.1.5:4001`); `--iface ADDR` then names the local interface
  address the datagrams leave from.
- `--tcp HOST:PORT`: connects, for a `tcp_listen` CAT021 input.
- `--tcp-listen PORT`: listens on 127.0.0.1 and serves one client at a
  time, for a `tcp_connect` CAT021 input (`"host":"127.0.0.1"` and that
  `"port"`). `--bind ADDR` listens on another local address instead, such as
  `0.0.0.0` when skycued runs on another machine.

`--as json` is UDP only, as the JSON input is. A capture is best replayed
over the transport it was recorded on: pingsim warns once when a record
captured over UDP goes out over TCP (a JSON datagram then has no framing),
or one captured over TCP goes out as a datagram. `--speed N` plays N times
faster (0: as fast as possible; over UDP the receiving socket may then drop
datagrams), `--loop` starts again after the end, and `--sac N` / `--sic N`
set the CAT021 I021/010 (default 0/0). Without `--loop` pingsim exits 0 once
everything is sent; with `--loop` it exits 1 when a pass sends nothing. A
usage error exits 2. The last line on stderr sums up: `done: N records sent,
M lines skipped, K status messages, ...`.

To demonstrate the Feeds tab with no readsb or dump978 running, put two
simulated inputs in an inputs file, `sim.inputs.json`:

```json
{"version":1,
 "inputs":[
  {"id":"sim-json","name":"Simulated (JSON)","type":"pingstation_json","bind":"127.0.0.1","port":4000},
  {"id":"sim-cat","name":"Simulated (CAT021)","type":"pingstation_cat021","transport":"udp","bind":"127.0.0.1","port":4001}
 ]}
```

then run skycued on it and one pingsim per input:

```sh
skycued --no-readsb --no-uat --inputs-file sim.inputs.json --config skycue.toml &

# JSON with a device status every 10 s: the card shows the device line
# (GPS 3D fix, receiver normal, firmware 1.0.0) and a device marker there.
skycue-pingsim --udp 127.0.0.1:4000 --as json --loop \
    --status-interval 10 --status-lat 38.9 --status-lon -77.0 \
    tests/fixtures/readsb-sample.jsonl &
# CAT021 from the same recording, five times faster.
skycue-pingsim --udp 127.0.0.1:4001 --as cat021 --speed 5 --loop \
    tests/fixtures/readsb-sample.jsonl
```

The fixture's aircraft are around 38.9 N, 77.0 W, so `receiver.lat` and
`receiver.lon` must be near there or the range filter drops them. The status
message carries guid `0000000000000001`, version 1.0.0, `gpsStatus` 3,
`receiverStatus` 0, the send time and the given position; `--status-interval`
needs `--as json` and both `--status-lat` and `--status-lon`.

**Replaying a capture.** Add `"capture":true` to the `sim-cat` entry above
(or a real device's input) and set `recording.dir` in skycue.toml (the example
config leaves it empty, and capture needs it): the input then writes
`recording.dir/capture-sim-cat-*.jsonl`. Play it back into the same kind of
input, and look inside it with `skycue-dump --capture`
([below](#watching-the-stream-skycue-dump)):

```sh
skycue-pingsim --udp 127.0.0.1:4001 --capture /data/rec/capture-sim-cat-20261102-101500.jsonl
skycue-dump --capture /data/rec/capture-sim-cat-20261102-101500.jsonl | less
```

A capture holds the device's own timestamps from when it was recorded:
replayed later they are outside the input's ±10 s clock window, so the input
counts them in `clock_rejects` and dates the positions by arrival instead.
Synthesised records (`--as`) carry the send time and are accepted.

## Recordings

With `recording.dir` set, skycued writes these streams there. The first two are
in the formats listed in `recording.formats` (`jsonl`, `csv`, or both):

- `received-YYYYMMDD-HHMMSS.{jsonl,csv}`: every observation from readsb and
  the pingStation inputs, and every UAT uplink, before filtering. The JSONL file is what `skycued --replay FILE` plays back.
- `sent-YYYYMMDD-HHMMSS.{jsonl,csv}`: every update and drop skycued
  forwarded, and the active filters at startup.
- `fisb-YYYYMMDD-HHMMSS.jsonl` (JSONL only, with `fisb.enable = true`): the
  decoded FIS-B weather.
- Per pingStation input, also directly under `recording.dir`:
  `capture-<id>-*.jsonl` for an input with `"capture":true`, and, with
  `recording.raw = true`, `raw-<id>-*.csv` (1090) and `raw-<id>-*.txt` (UAT) for a
  `pingstation_json` input ([the inputs file](#the-inputs-file)). Each of
  these is a stream of its own.

skycue-rawrec's raw captures (readsb's Beast output and dump978's raw lines)
belong under `recording.dir/raw`: run it with `--dir recording.dir/raw
--max-gb recording.max_gb --retention-dir recording.dir` ([above](#raw-capture-and-replay)).
Every file rotates at 256 MiB or after an hour; the stamp is the UTC time a
file was opened. The record layouts are in
[recording-format.md](recording-format.md).

### Disk planning

Set `recording.max_gb` to the disk space the recordings may use (say 80% of
the file system's free space): skycued, and skycue-rawrec when given
`--max-gb`, then delete the
oldest recordings under `recording.dir` and `recording.dir/raw` whenever they start
a new file, keeping the total within the budget (details in
[recording-format.md](recording-format.md#retention-record_max_gb)). Only
files with skycue's own names are counted or deleted, so a copy you want to
keep is safe once renamed or moved elsewhere. With `recording.max_gb = 0` (the
default) nothing is deleted and recordings grow without bound, so prune them
yourself (a cron job removing files older than N days, say). Allow for the
current files on top of the budget: up to 256 MiB per stream until the next
rotation, where every stream counts: received, sent and fisb, each of
skycue-rawrec's two raw files, and each pingStation input's capture and raw
files. Rough growth:

- **Received stream:** about 450 bytes per observation for JSONL and CSV
  together. readsb emits roughly one observation per aircraft per second, so
  100 aircraft in view is about 45 kB/s, 160 MB/hour, 4 GB/day.
- **Sent stream:** smaller, bounded by `cues.min_interval_s` per aircraft (at most
  one update per aircraft per `cues.min_interval_s`, plus keyframes every
  `cues.keyframe_interval_s`).
- **pingStation capture** (`"capture":true`): one line per datagram or TCP
  block, about twice the bytes received plus some 80 bytes. The raw frames of
  a `pingstation_json` input (`recording.raw = true`) are about 30 to 90 bytes per
  frame.
- **Raw Beast capture** (skycue-rawrec): about 15-25 bytes per Mode S
  message. A busy site decodes on the order of 1,500 messages a second
  (the bundled 120 s sample has about 177,000), which is about 30 kB/s,
  110 MB/hour, 2.5 GB/day. Raw UAT lines are far smaller.

If the disk fills, skycued's recorder and skycue-rawrec drop data and retry
with a backoff from 5 s to 300 s (see [recording-format.md](recording-format.md))
rather than stop, creating at most one extra file per stream however long it
lasts; cues keep being forwarded.

## Curated lists

`filters.allow_list_file` and `filters.block_list_file` name plain-text files, read when
skycued starts and again within about 2 s of an edit:

```
# one entry per line; '#' starts a comment
a1b2c3        # an address: six hex digits, any case
N123AB        # a registration (a US N-number also matches its ICAO address)
G-ABCD
```

A blocked aircraft is never sent. An allow-listed one is sent whatever its
range or elevation. An aircraft that is on neither list is sent if it is
within `filters.max_range_nm` and above `filters.min_elevation_deg`. An aircraft without a
position is never sent. A bad entry stops skycued with exit status 2, naming
the file and line.

## The web UI

skycued serves a live map on `web.port` (default 8080). Open
`http://<web.bind>:<web.port>/` in a browser: with the defaults that is
`http://127.0.0.1:8080/` on the receiver itself. `web.bind = 127.0.0.1` (the
default) only accepts connections from that machine; set `web.bind = 0.0.0.0`
in `skycue.toml` (and restart skycued) to reach the page from another
machine, as `http://<receiver address>:8080/`. There is no login: anyone who
can reach the port can change the filters and lists, so open it only to a
network you trust. `web.port = 0` turns the page off. The page loads nothing
from the internet; the API behind it is in [web-api.md](web-api.md).
The page's files are in `<prefix>/share/skycue/www` (`www/` in the source
tree; Leaflet under `vendor/leaflet/`, ECharts under `vendor/echarts/`).

The left panel has two groups: **Output filters** (what is sent to the
cue clients: range, elevation and the allow and block lists, see
[Filters and lists](#filters-and-lists)) and **Display filters** (this
browser only). The map settings (Trails, "Non-matching on the map", "Hide
filtered") are in the Aircraft box of the map toolbar, top left of the map:
see [The map toolbar](#the-map-toolbar).
"Hide filtered" (off by default, remembered per browser) leaves out of the
map and the table the aircraft that are not `sent` or `allow_listed`; the box
shows how many are hidden. It only changes what this page draws, not the
filters, lists, recordings or cue outputs.

The Display filters narrow what this browser's map and
table show, and nothing else. The search matches the hex, callsign, registration or squawk
(`/` puts the cursor in it, Escape clears it); altitude, speed and range
take a minimum and a maximum (the "ground" box says whether aircraft on
the ground are included; their altitude counts as 0); the pills pick
decisions (sent, allow, range, elev, blocked) and classes (airline,
private, military, unknown); "emergency only" keeps
aircraft declaring an emergency or squawking 7500, 7600 or 7700; Category
picks the emitter category and Country the country of registration.
"Non-matching on the map" (in the Aircraft box) either hides the other
aircraft or dims them to faint symbols without labels or trails; the table
leaves them out. The section shows "showing N of M" and a Clear button, and its heading the
number of active settings. The settings are remembered per browser
(localStorage `skycue.view`) and combine with "Hide filtered" (Aircraft box); the selected
aircraft always stays shown. The class and the country are estimates, from
the ICAO address, the callsign and the category (an address that is not an
ICAO address, such as a TIS-B track file, gets no country and no
address-based military class); the Track panel shows them as "Class (est.)"
and "Country". Like "Hide filtered", Display filters only changes what
this page shows, not the filters, lists, recordings or cue outputs.

**Saved views.** The "Views" row at the top of Display filters keeps
named views: every Display filter setting together with "Hide filtered" and
the four map boxes (the Map box's style, Borders and Cities, the Layers
box's layer set, and the Weather box's radar opacity). Choosing a view in
the list applies it at once; "— current —" leaves the settings as they are.
**Save** asks for a name (up to 40 characters) and saves the current
settings under it; a name already taken is replaced after a confirmation.
**Delete** (after a confirmation) removes the chosen view, and **⋯** holds
Rename, Move up, Move down, Export… and Import…. Once the settings no
longer equal the chosen view, the list shows "<name> (modified)"; choosing
the view again applies it again. A view only sets the map fields this
install can show (a view saved with Dark applied to an install without
tiles leaves the style alone) and never opens or closes a box. Views saved
before the map boxes existed have no map part: recalling one changes the
Display filters and "Hide filtered" and leaves the map as it is. The keys 1
to 9 apply the first nine views and 0 clears the Display filters to their
defaults, like Clear (not while typing in a field). Up to 20 views are kept,
in localStorage `skycue.viewSets` (version 2).

**Export…** downloads `skycue-settings-<date>.json` (the UTC date): the
views and the alert settings (the rules, their parameters, the watch
list and Mute; see [Alerts](#alerts)), so one file carries a whole setup
to another browser. The file is `skycue_settings` version 2 (views with
their map part); version 1 files, from before the map boxes, still import,
their views without a map part. **Import…** reads such a file (at most 1 MB), says
what it holds and which of your views a merge would replace, and offers
**Replace**, **Merge** and **Cancel**. Replace takes the file's views and
alert settings instead of yours (a file without alert settings leaves
yours as they are). Merge keeps your views in their order,
replaces the ones with a name the file also has, adds the file's other
views at the end up to 20 (the rest are named as not imported), takes the
file's rules one by one (a rule the file lacks stays as it is) and adds
the file's watch list to yours; your Mute stays. Either way the strip
stays shown or hidden, and its drawer open or closed, as it was. The views
and the alert settings are this browser's only: an import or an export is
never sent to skycued.

The header shows the counters: tracks known and forwarded, observations per
second, `inputs n/m` (enabled inputs connected / enabled inputs: green when
all are, else red; a click opens the [Feeds tab](#feeds-tab)), UAT
downlinks and uplinks per minute, cue clients, recorder drops and web
clients. To the right, **Weather**, **Feeds**, **Cues** and **Config**
open the side panel on that tab (the one whose tab is showing is
highlighted; a second click, or Escape, closes the panel). **Table**
swaps the map for the track table and **Stats** for the
[Stats view](#stats-view); while either is shown its button reads **Map**
and goes back. A red banner says when the page has lost skycued (the connection
closed, or no stats for 5 s); it reconnects by itself.

### Trails

The **Trails** group at the top of the Aircraft box (the toolbar's aircraft
button, see [The map toolbar](#the-map-toolbar)) sets how the aircraft
trails are drawn. Its settings are remembered per browser with the rest of
Display filters (localStorage `skycue.view`), are part of the saved views
(and so of the number keys 1 to 9), and Clear (or the 0 key) puts them back
to their defaults; they do not count in the Display filters heading's number
of active settings. The trail length is `web.trail_seconds` in every setting.

**Show**: `all` (the default) draws a trail for every aircraft on the map;
`selected` only for the selected aircraft; `off` none. The positions are
kept either way, so selecting an aircraft, or switching back to `all`,
shows its whole trail at once.

**Colour by**: `decision` (the default) draws each trail in its aircraft's
decision colour (see [The map and its colours](#the-map-and-its-colours)).
The other two draw it in pieces, each stretch in the colour of the
position it leads to, with a small scale under the control showing the
colours and their bounds (hover a colour for its range):

- `signal`: the signal strength (RSSI) each position was received at, on a
  fixed scale of six 5 dB steps from −35 dBFS or weaker (dim blue) through
  violet, magenta and red to −5 dBFS or stronger (bright orange). Weak
  stretches show where the antenna's coverage thins out (terrain,
  obstructions, the edge of range). readsb reports −49.5 dBFS when it has
  no signal level for the data (replayed recordings, network-fed Beast or
  raw inputs), so positions at −49.5 or lower count as "no signal", and
  the track detail's RSSI row shows "—".
- `altitude`: the barometric altitude of each position (the geometric
  altitude when there is no barometric one; an aircraft on the ground
  counts as 0), in seven steps: ground to 1,000 ft (dark green), to
  5,000 (olive green), to 10,000 (dark amber), to 20,000 (orange), to 30,000
  (red), to 40,000 (magenta), above 40,000 ft (violet). A bound belongs
  to the step below it (exactly 5,000 ft is the second step).

A stretch without a value (no signal, or no altitude) is drawn solid in a
faint neutral grey, a colour on neither scale nor among the decision
colours, so it cannot be read as a step; a trail with no signal anywhere
(a replay, a network feed) is a faint grey line in `signal`. The legend's
last line shows that grey. Every step of the altitude scale is at least
3:1 against the Light map's land. Trails are always solid lines; the
only dashed line from an aircraft's symbol is its projection ahead
(dashed airspace outlines are something else: cancelled shapes).

A view saved before the Trails group existed takes the Trails
settings the page had when it was first opened with this version (once),
so applying it keeps them rather than resetting Trails.

**Project ahead**: `off` (the default), `1`, `2` or `5` minutes. Each
aircraft with a trail also gets a dashed line in its decision colour from
its symbol along its track, as far as it flies in that time at its
current ground speed (480 kt for 2 minutes is 16 nm), on the great circle
from its position. It moves with every update and is not drawn for an
aircraft without a track or a ground speed, or on the ground. It follows
Show (none when `off`, the selected aircraft's only when `selected`). This
is dead reckoning for straight, level-speed flight only: a turn, a climb
or a speed change is not predicted, so an aircraft in a turn leaves its
line at once.

### The map and its colours

Each aircraft is a plane symbol pointing along its track, labelled with its
callsign (or registration, or hex), with a trail of the last
`web.trail_seconds` of positions. A position the aircraft could not have flown
to (over 1500 kt from the trail's last point, taken over at least a second,
or more than 5 s earlier than it: a looped replay starting again, a bad
position, a reused hex) starts the trail afresh there instead of joining it;
one up to 5 s earlier (timestamp rounding, two inputs out of order) is left
out of the trail. The colour is skycued's decision for it:

| Colour | Decision | Meaning |
|--------|----------|---------|
| green | `sent` | within range and elevation: cues are sent |
| blue | `allow_listed` | on the allow list: sent whatever its range or elevation |
| grey | `out_of_range` | farther than `filters.max_range_nm`: not sent |
| orange | `below_elevation` | lower than `filters.min_elevation_deg`: not sent |
| red | `blocked` | on the block list: never sent |
| purple | anything else | a decision this page does not know (a newer skycued) |

A marker glows briefly each time a cue is sent for it. The blue dashed
circle is `filters.max_range_nm` around the receiver (the white symbol); the orange
dotted circle is where an aircraft at the altitude chosen under "Elevation-
mask ring for aircraft at" sits exactly at `filters.min_elevation_deg`: inside it,
an aircraft at that altitude is above the mask. Aircraft without a position
are only in the table.

**Measuring a distance.** The ruler button (title "Measure") under the
zoom buttons, above the box buttons, arms the ruler. Click the map (or an aircraft, a weather
symbol or a ground station, to use its position) for the start; as the
mouse moves a dashed line follows it, labelled with the great-circle
distance in nm and km and the initial bearing in degrees true (`066°T`);
a second click fixes it, and a further click starts a new measurement. The
line is drawn straight on the map; the figures are great-circle, which
over a few hundred nm is a slightly different path. On a touch screen
nothing follows the finger: tap the start, then the end. Escape or the
button again clears it and disarms, and so does switching to the Table or
Stats view. While the ruler is armed, clicks on the map, an aircraft or a
weather shape or symbol do not select anything or open the Weather tab,
and double-click does not zoom.

### Selecting an aircraft

Click a marker (or a table row) to open the panel on the right with every
field skycued knows for it: identity, position and its age, altitudes,
speed, range, bearing, elevation, decision, signal quality, first and last
seen. Its buttons add the aircraft's hex address (and, when known, its
registration) to the allow or block list, or remove it when it is already
listed. Close the panel with the × button, Esc, or a click on the map.

### Filters and lists

On the left, under **Output filters**:

- **Max range (nm)** and **Min elevation (°)**: type a new value and press
  Enter (or leave the field) to apply it, to every aircraft at once. While
  you type nothing is sent; a value out of range is marked in red with a
  message under the inputs and is not applied. The ↺ button goes back to
  the `skycue.toml` value.
- **Allow list** and **Block list**: the entries in force, each with a ×
  to remove it; type a hex address or registration and press Add (or
  Enter) to add one.

These changes do not touch `skycue.toml` or the list files: they go to
`skycue.runtime.json` (the "overlay"), in `paths.state_dir` when that is set, else
next to `skycue.toml`, which skycued also reads at startup, so they survive
a restart. While the overlay holds
anything, "Unsaved changes: the running filters differ from the baseline and
the list files" is shown and **Save as baseline** is enabled. Save as
baseline (after a confirmation) writes the running values into the list files
and into `skycue.overrides.toml`, and clears the overlay. skycued never
writes `skycue.toml`: the overrides file is its own, written whole, in
`paths.state_dir` when that is set, else beside `skycue.toml`, and skycued
reads it after `skycue.toml`, so its values win
(see [Where the files are](#where-the-files-are)). Where that directory is
not writable (the packages' `/etc/skycue`, for a hand install that left
`paths.state_dir` empty) the save fails with a message that names the path and
says to set `paths.state_dir`. A filter key that the environment or `--set`
pins is skipped. What could not be saved
(for instance a list
whose `filters.allow_list_file` or `filters.block_list_file` key is empty) is listed under the button and stays in the
overlay. The details, and what happens on a hand edit of these files while
skycued runs, are in [web-api.md](web-api.md#runtime-overlay-skycueruntimejson).

### Table view

**Table** in the header swaps the map for a table of every track (hex,
callsign, registration, altitude, ground speed, range, elevation, decision),
sorted by a click on a column heading (again for the reverse order). A click
on a row selects that aircraft; **Map** goes back. **Stats** beside it
shows the history skycued has gathered instead: the
[Stats view](#stats-view).

### Alerts

The alert strip runs along the bottom of the main area, over the map, the
table and the Stats view. It shows the newest unacknowledged alert on one
line (its time in UTC, its kind, the aircraft or input, a detail such as
"squawk 7700"), with the number of unacknowledged alerts on the left (red
while one of them is an emergency) or "No unacknowledged alerts". On the
right: **Ack** acknowledges the alert shown, **Ack all** every one, **▲**
opens the history drawer (the last 200 alerts, newest first, each
unacknowledged one with its own Ack; acknowledged ones are dimmed), **⚙**
the alert settings, and **×** hides the strip. **Alerts** in the header
shows or hides it again and carries the number of unacknowledged alerts.
The page works the alerts out itself, from the tracks, the drops, the
inputs and its own connection to skycued: nothing is sent to skycued, and
every browser has its own alerts.

The rules, in the order of the settings panel:

| Rule | Fires when | Parameters | Default |
|------|------------|------------|---------|
| Emergency | an aircraft squawks 7500, 7600 or 7700 or declares an emergency | — | on, sound on (double beep) |
| Military | an aircraft estimated military is seen for the first time | — | on |
| Sent | an aircraft is `sent` or `allow_listed` for the first time | — | on |
| Input lost | an enabled input has been disconnected for 10 s, or the page has lost skycued for 10 s | — | on, sound on (double beep) |
| Range | an aircraft comes closer than the range | "within nm" 25 (0.1–500) | off |
| Low altitude | an airborne aircraft is below the altitude within the range | "below ft" 1000 (0–60000), "within nm" 10 (0.1–500) | off |
| New country | the first aircraft from a country of registration not seen before | — | off |
| Watch | an aircraft matches an entry of the watch list | the watch list | off |
| Filtered drop | skycued drops a forwarded aircraft by a filter decision | — | off |

"For the first time" and "not seen before" mean since the page was
opened (a reconnect does not start them again). A rule fires on a change, from not met to met: a condition that
stays true never fires again. Each rule gives at most one alert per
aircraft (or input) until it is acknowledged; after the acknowledgement the
same aircraft raises that rule again only on a new change at least 5
minutes later. A change inside those 5 minutes is listed in the drawer,
marked "quiet period", but neither raises the strip nor sounds.

When the page opens, and after every reconnect, the aircraft already there
are a silent baseline: they are seen and their range and altitude noted,
but Military, Sent, Range, Low altitude, New country and Filtered drop do
not fire for them. Emergency and Watch do: an aircraft already squawking
7700 when the page opens is shown at once.

Input lost waits 10 s (the hold-off) before it fires, so an input that
reconnects within that time raises nothing; the same holds for the page's
connection to skycued (the red banner), which fires as "skycued:
connection lost". When the connection comes back the alert is acknowledged
by itself and its drawer row gains "recovered after <time>" (an alert left
open by an earlier page shows "recovered"). An input disabled or removed
meanwhile ends it with "input disabled" or "input removed". An input seen
for the first time is only noted: one that is already down when the page
opens raises nothing. After a reconnect, an input the page last saw
connected and now sees disconnected starts its hold-off, and a hold-off
already running when the connection dropped goes on through the reconnect.

Browsers play sound only after a click on the page: until the first click
the strip shows "🔕 click to enable sound". A rule with its sound on
beeps when it raises an alert: Emergency and Input lost twice, the others
once. One tone at most sounds for each update, the double beep when any
alert it raised has one. "Mute all
alert sounds" in the settings silences every rule.

**⚙** opens "Alert settings" over the drawer: one row per rule with its
"on" and "sound" boxes and its parameters (a value outside the limits is
marked in red and not applied); the "Watch list (hex, callsign or
registration prefix, one per line)", matched case-insensitively against
the start of the hex, the callsign or the registration (at most 200
entries of 16 characters); "Mute all alert sounds"; and **Clear history**,
which asks first ("Clear the alert history? Acknowledged and
unacknowledged alerts are removed.") and empties the drawer. A condition
still true then does not fire again. A changed parameter or watch list
applies to the aircraft already there without firing. Every change is kept
at once; Escape or **×** closes the panel.

A click on an alert (the strip's line, or a drawer row) goes to what it is
about. An input's alert (and skycued's) opens the
[Feeds tab](#feeds-tab). An aircraft's switches to the map and
selects it, even when the Display filters leave it out; when "Hide
filtered" would hide it, Hide filtered is turned off and the strip says
"Hide filtered turned off to show <aircraft>.". An aircraft no longer
tracked gets "<aircraft> is no longer tracked." and, when the Stats ledger
has it, an **Open in Stats** button that opens its detail in the
[Stats view](#stats-view).

All of this is kept in this browser only: the rules, their parameters, the
watch list, Mute and whether the strip and the drawer are shown in
localStorage `skycue.alerts`, and the last 200 alerts with their
acknowledgements in `skycue.alertLog`, so a reload neither raises the same
alerts again nor loses the unacknowledged ones.

### Stats view

**Stats** in the header swaps the map for charts of what skycued has heard
since the statistics began (or were last reset); **Map** goes back.
skycued keeps these statistics itself, in the
[statistics file](#statistics-file), so they cover the time no page was
open, and every browser sees the same ones. The page reads them from
`/api/stats/*` ([web-api.md](web-api.md#statistics)) only while the view is
shown.

The toolbar along the top:

- **1 h**, **6 h**, **24 h** (the default), **7 d**, **30 d**: the time
  range, remembered per browser. Up to 24 h the time charts have one bar
  per minute, beyond that one per hour (minutes are kept for 24 h, hours
  for 30 days).
- **live** (on by default): refresh every 10 s while the view is shown.
  Off, the charts stay as they are until a range is picked again.
- "since 2026-11-03 12:00Z": when the statistics began, in UTC like every
  time in the view.
- **Feeds** opens the [Feeds tab](#feeds-tab).
- **Reset statistics** asks first ("Reset all statistics? ... this cannot
  be undone."), then empties all of it (series, charts, coverage, the
  aircraft ledger and its evicted count) and saves the empty file at once.
  The header counters and `GET /api/stats` are the process's own counts and
  are not touched. Another reset within 5 s of the last one is refused
  ("Reset failed: the statistics were reset less than 5 s ago").

The first Traffic card, **Flow**, is a live diagram of the pipeline: one band
per input flows into the decoder, then to rejected messages and track
updates, which split by decision (sent to cues, out of range, below
elevation, blocked, no position); the sent band goes through the cue
scheduler to one node per cue connection. Band widths are messages per
second over the last minute, and the cue connections show the copies after
the scheduler (one per connection). A thin band carries nothing. A ● marks a
node a recording file is written from (hover for the file, its size and
write rate) and ↗ one a raw output serves. Click an input or a cue
connection to open its card in the [Feeds tab](#feeds-tab). It
refreshes every 5 s while **live** is on and the page is visible, and its
**CSV** button saves the rates.

Below it, the sections:

- **Tiles**: aircraft (and how many were new), observations (and
  positions), sent cues, drops (the timeout, filtered and source-lost drops
  sent), the maximum range and the aircraft that made it, and emergencies,
  each with a sparkline over the range.
- **Traffic**: observations per bucket stacked by input (the slider under
  it zooms), distinct aircraft per bucket, and sent updates against drops
  in the decision colours. "Busy hours of the week (UTC)" is a heatmap of
  the hourly series folded onto one week, Monday to Sunday by UTC hour, so
  the same hour of every week adds up; its caption names the busiest hour.
- **Coverage**: the farthest range heard in each 5° bearing sector, per
  altitude band (a click on a bar opens the aircraft that made it in the
  table below); histograms of range and elevation; and a map of where
  positions were heard, in 0.05° cells. Range, elevation and the sectors
  need `receiver.lat`/`receiver.lon`.
- **Fleet**: histograms of altitude and ground speed; the observations by
  emitter category, source and address type; the aircraft by estimated
  class and by country of registration. A click on a category slice, a
  class slice or a country bar sets that setting in Display filters and
  goes to the map (Clear there undoes it).
- **Signal**: signal level against range (a heatmap, with the median level
  of each 10 nm ring as a line) and a histogram of signal levels.
- **Weather (FIS-B)**: UAT uplink and downlink frames and FIS-B products per
  bucket. It is shown only once a UAT observation, frame or product has
  been counted since the last reset.
- **Aircraft**: the ledger of every aircraft heard, 100 rows a page, sorted
  by a click on a numeric or time heading (again for the reverse order) and
  searched by hex, callsign or registration. A click on a row (or Enter)
  opens its detail: every field the ledger holds and a small polar plot of
  where it was heard farthest. **Show on map** (only while the aircraft is
  a live track) selects it on the map; **Filter the map to it** puts its
  hex in the Display filters search and goes to the map.

The tiles, the Traffic and Weather charts and the class and country charts
follow the time range. The heatmap always covers the whole hourly series
(the last 30 days). Everything else (the altitude, speed, range, elevation
and signal histograms, the category, source and address-type counts, the
coverage and the signal heatmap) is whole-history; their captions say
"since ...". The aircraft table is the whole ledger. "Class" and "country"
are estimates, by the Display filters' rules (from the ICAO address, the
callsign, the registration and the category; an address that is not an
ICAO address gets no country).

The ledger holds at most `stats.aircraft_max` aircraft (default 20000).
Past it the 5 % heard longest ago are forgotten, and the table says how
many under it: "and N older aircraft (dropped from the ledger when it was
full)". A forgotten aircraft heard again gets a new entry and counts as new
again.

Three cards have a **CSV** button that downloads what they show as a file:

- **Aircraft ledger**: every ledger row matching the search, in the table's
  sort, read 1000 rows at a time ("Exporting… N rows" meanwhile), as
  `skycue-aircraft-<date>.csv`. One column per ledger field (the
  [ledger fields](web-api.md#statistics), units in the names where the
  field's own name has none, such as `max_range_lat_deg` and
  `min_rssi_dbfs`); the callsigns, squawks and inputs are separated by
  spaces. An aircraft is in the file once, even when the ledger grows
  while the export runs.
- **Observations by input** (Traffic): the series of the time range shown,
  one row per minute or hour (`time`, every column of the series, then
  `obs_<input>` for each input), as
  `skycue-observations-by-input-<date>.csv`.
- **Range by bearing and altitude** (Coverage): one row per 5° sector and
  altitude band, 72 × 8 rows (`sector_start_deg`, `band`, `max_range_nm`,
  `holder`, `time`, `n`; an empty cell has `n` 0), as
  `skycue-coverage-<date>.csv`. It needs `receiver.lat`/`receiver.lon`.

`<date>` is the UTC date of the data clock. The files are RFC 4180 CSV
(a header row, quotes where a cell needs them, CRLF line ends) in UTF-8
without a BOM, built in the page. Every chart card also has a small
"Save as PNG" icon at its top right, which saves the chart as
`skycue-<chart>-<date>.png` on the page's dark background.

For recordings, `skycue-stats`
([below](#analysing-recordings-skycue-stats)) still gives the same kind of
numbers offline, from the files alone.

### Statistics file

skycued keeps the statistics in `stats.file`, by default
`skycue.stats.json` beside the overlay, `skycue.runtime.json`: in
`paths.state_dir` when that is set, else next to `skycue.toml`. A relative path
is taken from `paths.state_dir` when it is set, else from `skycue.toml`'s
directory. It reads the file at startup and logs `<file>: statistics of N
aircraft since <time>`. It writes the file every `stats.save_s` seconds
(default 300) when anything changed, at a clean stop, and at once on a
reset, so a crash loses at most the last interval. A failed save logs one
WARN and is retried every interval. `stats.file = ""` keeps the
statistics in memory only: they start empty at every start. All three keys
are read at startup: a change needs a restart.

At the defaults a full file (20,000 aircraft, a full position grid, 24 h of
minutes and 30 days of hours) is about 6 MB, and reading it at startup
takes about 150 MB of memory for a moment. A file that cannot be read back
(malformed, a version this skycued does not know, or over 32 MiB) is
renamed `<file>.bad`, replacing an older one, with one ERROR line, and the
statistics start empty; the next save writes a new file. A `stats.file`
that is not a regular file (a directory, `/dev/null`) is left alone: one
ERROR line, and the statistics are kept in memory only for that run. A save
that writes more than 24 MiB logs a WARN (at the 100,000-aircraft maximum
the file is roughly 27 MB, close to the 32 MiB a load accepts). A file with more
aircraft than `stats.aircraft_max` (after lowering it) loses the oldest at
load, counted as evicted.

Every time is skycued's data clock (the newest observation's time), so a
replay builds a history in the recording's time. A replay of a recording
older than the newest bucket (after a live run, say, or each pass of
`--loop`) restarts the time series: the minute and hour charts start again
from the recording's time and skycued logs `stats: the data clock moved
backwards to <time>; the series restart`, while the histograms, coverage,
the grid, the counts and the ledger keep counting. A live run after a
replay goes on from the replay's time: the gap shows as empty buckets, and
a gap longer than 24 h (the minutes) or 30 days (the hours) starts that
series afresh. After each start of skycued (and each restart of the
series) an aircraft already counted in the open minute or hour can be
counted in that bucket's aircraft once more.

### Weather tab and weather reports

The side panel's second tab (also opened by **Weather** in the header) lists
the FIS-B text reports skycued holds: METAR, TAF, PIREP, winds aloft and the
NOTAM / SIGMET reports without graphics, with a station-prefix filter and
age colouring. The **Reports** toggles (METAR, TAF, PIREP, Winds, each with
its count) and the filter apply to the list and to the map symbols alike.

On the map a METAR is a circle coloured by flight category (VFR green, MVFR
blue, IFR red, LIFR magenta) with a wind barb, a TAF a square beside it, a
PIREP a triangle ("!" for an urgent one), and winds aloft a barb with the
temperature for the picked forecast period (6, 12 or 24 h; default the one
nearest the data clock) and level (default the lowest). Hover names the
station; a click shows the parsed report and its raw text. A report shows
on the map only when its station's position is known
([Report locations](#report-locations-fisb-locationsjson)); otherwise the
list says "no position". At most 300 symbols are drawn, those in view.

The weather layers are in two map boxes. The **Layers** box holds the
checklist: "Radar (regional)" and "Radar (CONUS)" (on by default), "Other
rasters", and one layer per airspace shape family (NOTAM / TFR, SIGMET,
AIRMET, CWA, SUA; off by default). The set is remembered per browser
(localStorage `skycue.layers`) and the number of layers on shows on the
Layers button. "Other rasters" shows one more FIS-B raster at a time
(lightning, icing, turbulence, cloud tops): the **Weather** box then offers
a product dropdown, and icing and turbulence add an altitude band dropdown
(default the lowest band with data). The winds and raster picks stay until what they name is no longer
offered; a reconnect, which reloads everything, keeps them.

The **Weather** box (the cloud button) holds the radar's age, the "Radar
opacity" slider, the radar and airspace legends and the raster controls.
The slider sets how opaque the NEXRAD layers ("Radar (regional)" and "Radar
(CONUS)") and the "Other rasters" product are drawn, from 0.2 to 1 (default
0.7); a radar older than 15 min (regional) or 30 min (CONUS) is drawn at half
of it. The slider is disabled, and dimmed, while none of these is drawn (no
mosaic on a layer that is on, no raster product). Its value is remembered per
browser (localStorage `skycue.radarOpacity`) and is part of a saved view. A
dot on the Weather button, with the box closed, says that a radar layer that
is switched on is stale (drawn at half opacity), or has no mosaic while the
daemon has FIS-B ground stations.

### Feeds tab

The side panel's third tab, after Track and Weather; **Feeds** or
`inputs n/m` in the header opens and closes it. A summary line at the top
counts what is flowing, e.g. `3 inputs up · 1 stale · 2 outputs · 4 clients ·
recording 2 files` (parts that are 0 are left out; an enabled input is up when
it is connected and receiving, stale when connected with no data, else down;
`no feeds` when no input is enabled). It has five sections.

**Inputs**: one card per input (the built-ins first, then the added ones in
file order) with its name, id and type, where it gets its data
(`udp 0.0.0.0:4000`, `tcp → 192.0.2.10:4000`, `tcp listen 0.0.0.0:4000`,
`readsb 127.0.0.1:30047`), a status dot, how long ago data last arrived,
observations and messages per second (from the once-a-second counters),
the rejected count (with `rejected_peer` beside it when data came from an
address other than the input's `peer`) and the latest error. The dot is green when the input is
connected and data arrived in the last 30 s, amber when it is connected but
nothing came for 30 s, red when it is not connected or its device's status
poll failed 3 times in a row, grey when it is disabled. A UDP input is
"connected" as soon as its socket is bound, so amber is the sign that a
device has gone quiet. A pingStation card adds the device line: the GPS
status (`no GPS`, `no lock`, `2D fix`, `3D fix`, `DGPS`), the receiver status
(`normal`, `comm errors`, `not transmitting`; a code the receiver's interface document does not list is
shown as its number), the firmware, and the age of its last status, marked
*stale* once that is older than 3 status intervals. The device's reported
position is a diamond on the map (the receiver is the ring); **Show** pans
to it at the zoom shown, or, from a zoom below 6 with the device out of
view, centres it at zoom 6. **Disable**/**Enable** and **Remove** (after a
confirmation) take effect at once and are written to `skycue.inputs.json`;
**Edit** opens the form below. A built-in (`readsb`, `dump978`, `replay`) only
has Disable/Enable. While skycued replays a recording it refuses every
change, and the tab says so under the cards.

With CAT021 the device sends nothing but aircraft, so with an empty sky its
status poll (`status_url`) is the only sign that it is alive: give a CAT021
input a status URL. Give the device's IP address there, not a hostname:
skycued resolves names on one thread shared with its other outgoing
connections, and a lookup that hangs (a nameserver that does not answer)
cannot be cut short. While an earlier lookup is still outstanding skycued
skips the poll and counts it in `status_fail` with the last error
"resolve still pending".

**Add input** at the bottom of the inputs: pick the type and the form shows
that type's fields (for CAT021 also the transport, which decides between
Host and Bind). An empty field means skycued's default. The form checks the
same rules as skycued before it sends anything and marks the fields that
break them; what skycued still refuses (a port another input listens on, a
taken id) is shown under the form in skycued's own words. **Edit** fills the
form with an input's definition (the id cannot change) and **Save** replaces
it, which restarts the input; its counters carry on. Enabled follows the
card's Disable/Enable while the form is open, so Save never undoes it.
An input removed and added again under the same id starts as a new one.

**Outputs**: one card per output (the built-in cue outputs first), with its
name, id and kind, where it serves or sends (`tcp_listen 0.0.0.0:30105`,
`tcp_connect 192.0.2.30:30500`, `udp 239.255.10.5:4978 via 192.0.2.10 ttl 1`), a status dot (green when it
is up, amber when it is up but has an error, red when it is not, grey when it
is disabled) and its counters: `peers`, `bytes`, `dropped` and the latest
error. **Disable**/**Enable** work on every card; **Edit** and **Remove** only
on outputs you added. **Add output** at the end of the section opens the form:
kind, id, name, transport and the fields of that transport; the multicast
interface, TTL and loopback appear once a group is typed. The form checks the
same rules as skycued and shows skycued's own words for what it still
refuses (a port in use, a loop). The counters refresh once a second while the
tab is open. The Add input form now offers the raw types and, for every type,
the transport and, for UDP, the multicast group.

**Cue clients**: every open cue TCP connection, with its endpoint,
`accepted` (it connected to `cues.tcp_port`) or `dialled` (one of
`cues.tcp_connect`), when it connected and the bytes written to it, then
every UDP target with the datagrams sent to it. The list is refreshed every
5 s while the tab is open.

**Ground stations**: the FIS-B ground stations heard in the last 60 minutes,
with position, products received and when last heard; each is a triangle
on the map.

**Decoder health**: the FIS-B decoder's records held per kind, bad frames,
segmented products waiting, mosaic blocks, the last uplink's time, uplinks
dropped as duplicates of another input's, and the recorder's dropped
records (received / sent).

### Cues tab

The side panel's fourth tab (**Cues** in the header opens and closes
it): what skycued sends to the cue consumers (the clients are on the [Feeds tab](#feeds-tab)). It has three sections.

**Counters**: updates and drops sent since skycued started, the drops by
reason (timeout, filtered, source lost) and the filtered drops by the
filter decision that dropped the track (blocked, out of range, below
elevation, no position; they add up to the filtered count). They update
once a second.

**Recent decisions**: the last 200 updates and drops the page received,
newest first, with the time (skycued's data clock), address, callsign,
filter decision and reason (`sent` for an update; `timeout`, `filtered` or
`source_lost` for a drop, with the decision only for `filtered`). The page
keeps them from the moment it loads, whichever tab is open; a reload starts
empty. A click on a row selects that aircraft on the Track tab. **Drops
only** (off by default, remembered in this browser) lists the last 200
drops alone, kept apart from the updates, so max-interval resends do not
push them out.

**Filter changes**: the last 100 changes of the range and elevation filters
(kept in skycued's memory, lost on restart), newest first, each with its
time (the wall clock, also during a replay, unlike Recent decisions'
data-clock times), origin (`startup`, `ui`, `file:<name>`, `save-baseline`) and what
changed, e.g. `14:10:41Z ui: max range 150 → 20 nm`. The oldest entry is
the state skycued started with. List changes are not in it.

### Config tab

The side panel's fifth tab (**Config** in the header opens and closes it).
Shows the configuration skycued is running with (`GET /api/config`, see
docs/web-api.md); nothing on it can be changed. Each `skycue.toml` key, by its
dotted name, has its running value and its **source**: `conf` (set in
`skycue.toml`), `default` (not set there, so the built-in default applies),
`overrides` (set in `skycue.overrides.toml`, which save as baseline writes,
and whose values replace `skycue.toml`'s), `env` (a `SKYCUE_*` environment
variable), `--set` (a `--set key=value` argument), `flag` (a command-line
option such as `--web-port`) or `runtime` (a range or elevation filter
changed in the UI and not yet saved as baseline). A key set by `env` or
`--set` is pinned: the page shows it read-only, and a filter key that is
pinned cannot be changed from the page. Keys marked restart take effect only
when skycued restarts; the four filter keys (`filters.max_range_nm`,
`filters.min_elevation_deg`, `filters.allow_list_file`,
`filters.block_list_file`) apply at once. Above the keys are the
daemon's version, uptime, the config, inputs, overrides and overlay files (and
whether the overlay holds unsaved changes), the www and recording
directories with the files being recorded, the log level, how it runs
(**Runs**: a replay of a named file with its speed and looping, or live and
whether it connects to readsb and dump978, so a replay or `--no-readsb` /
`--no-uat` is plain even though the decoder keys still show their values),
and the report locations it holds: **Learned locations** (how many and the
`fisb-locations.json` they are kept in, or "not saved this run") and
**Operator locations** (how many were read from the resolved
`paths.locations_file`, with any rejected lines and ids over the cap).

The keys are grouped as Receiver site, Decoders, Cue outputs, Recording,
Filters, Web, Statistics, FIS-B and Paths (a key skycued has that none of
these names is under Other). The tab asks skycued again each time it is opened
and on **Refresh**. **Copy as text** puts the whole configuration on the
clipboard in `skycue.toml` form: a `# <group>` line before each group, a
`[table]` line where the table changes, and before each `key = value` line a
comment naming its source (and `, restart`), e.g. `# filters.max_range_nm:
runtime` then `max_range_nm = 20`. Strings are quoted, booleans are
`true`/`false`, lists are arrays, so the text can go into a `skycue.toml` as it
is and skycued accepts it. A value the file cannot hold is left as a comment
saying so, with the value, to set by hand. Where the browser does not allow
the clipboard (a page served over plain HTTP from another machine, for one),
the tab says "Copy failed" and shows the text selected in a box to copy by
hand.

### Map tiles (optional)

Without tiles the map is a dark background with a 1°/5° latitude/longitude
grid and a scale bar, which is all the page needs. For coastlines and
terrain underneath, make tiles on a connected machine with
`scripts/maps/make-tiles.sh` and put them in `tiles/` under the map directory:
`paths.map_dir` when it is set in `skycue.toml` (`/var/lib/skycue/maps` in the
packages), else `<prefix>/share/skycue/www/` (the `--www-dir` directory in a build-tree run). The overlays
(`overlays/`, below) go in the same directory. With `paths.map_dir` set,
`/tiles/`, `/overlays/` and `/api/tiles-available` are read from it only,
never from the www directory, and a `paths.map_dir` that does not exist is
logged as a WARN at start and the map has no tiles or overlays. See
[make-tiles.md](make-tiles.md) for the sizes. The page picks them up on its next load.

With the packages, or a source install of `skycue/`, `skycue-make-maps`
makes both the tiles and the overlays straight into `paths.map_dir` (read from
`/etc/skycue/skycue.toml`, or `--config FILE`, or given as `--map-dir DIR`):

```sh
sudo skycue-make-maps --around 150                   # the world coarse, 150 nm around the receiver fine
sudo skycue-make-maps --preset conus                 # add the contiguous US at middle zooms
sudo skycue-make-maps --around 51.5 -0.12 100 --dry-run   # another point; print the plan only
sudo skycue-make-maps --container --preset europe    # GDAL from a container image (podman or docker)
skycue-make-maps --list                              # the regions already there
```

`skycue-make-maps [--config FILE] [--map-dir DIR] [--cache DIR] [--container [IMAGE]] [--owner USER] [--dry-run]`
with one of `--preset conus|europe|world`, `--around RADIUS_NM` (the
receiver's `receiver.lat`/`receiver.lon` from the config), `--around LAT LON
RADIUS_NM` or `--bbox W,S,E,N --zoom MINZ-MAXZ`; or `--list`. Runs add up:
each first bakes the whole world at zoom 0–5 (region `global`) unless
`tiles/meta.json` already has it, then the region asked for (`--around`:
zoom 6–9, at most 500 nm, the box clamped at the antimeridian and at ±85°
rather than wrapped, see make-tiles.md; `conus` and `europe`: zoom 6–8;
`world`: the base alone), then the overlays, with `scripts/maps/make-tiles.sh` and
`scripts/maps/make-overlays.sh` from `<prefix>/share/skycue/scripts/`. It needs a
network connection for the downloads (about 310 MB, kept in the cache:
`/var/cache/skycue` as root, else `~/.cache/skycue`). Without GDAL on PATH it
stops with exit status 3 and names both ways to get it: the distribution's
packages (`gdal-bin python3-gdal` on Debian and Ubuntu; `gdal python3-gdal`
from EPEL on RHEL-family systems), or `--container [IMAGE]` (default
`ghcr.io/osgeo/gdal:ubuntu-small-3.9.3`). It refuses a map directory that
is `/` or that already holds other files but neither `tiles/` nor
`overlays/` (exit 2), checks that it can write the map directory before
downloading anything (exit 1 otherwise), and as root hands the result to
the `skycue` user (`--owner` for another). Exit status:
0 done, 1 failure, 2 usage or config error, 3 a missing tool. With a
source tree, run `scripts/maps/make-tiles.sh` and `scripts/maps/make-overlays.sh`
from it as above (`scripts/maps/skycue-make-maps --map-dir DIR` works
there too, with the scripts beside it).

### The map toolbar

One vertical toolbar sits top left of the map: zoom in, zoom out, the
Measure button (see [Measuring a distance](#the-map-and-its-colours)), a
divider, then four icon buttons: **Map**, **Aircraft**, **Layers** and
**Weather**. Each opens a box to its right; one box is open at a time,
clicking the open button (or another one) closes it, and Escape closes it
when focus is inside the box (the ruler, when armed, takes Escape first).
A box is shortened to stay above the alert strip. The open box is remembered
per browser (localStorage `skycue.mapBox`); a browser that has not stored
one starts with all the boxes folded.

- **Map**: style, Borders and Cities, below. The button is left out when the
  install has neither tiles nor overlays.
- **Aircraft**: Trails (see [Trails](#trails)), "Non-matching on the map"
  and "Hide filtered" with the number of aircraft it hides.
- **Layers**: the overlay checklist, with the number of layers on shown on
  the button.
- **Weather**: radar age and opacity, the legends and the raster controls
  (see [Weather tab and weather reports](#weather-tab-and-weather-reports)).

### Map style and overlays

The Map box sets how the map looks; every choice is
remembered per browser, is part of a saved view, and only changes what this
page draws.

- **Map: Light / Dark.** Light shows the tiles as they were made; Dark
  darkens them with a CSS filter on the tile layer only (inverted, hue turned
  back, dimmed), so sea and land stay apart while the aircraft, range rings,
  weather and airspace keep their colours. Without tiles the background is
  already dark, so the select is left out (and the overlays are drawn in
  the Dark colours).
- **Borders** (off by default): country borders at every zoom, and US state,
  Canadian province and Mexican state lines from zoom 5, as thin lines, dark
  on Light and light on Dark.
- **Cities** (off by default): a dot and a name for the larger places, more
  as you zoom in: Natural Earth scalerank 0–2 up to zoom 4, 0–4 at zoom 5–6,
  all from zoom 7 (the file holds the places of scalerank 0–7 worldwide plus
  every place of 50,000 or more in the US, Canada and Mexico), at most 300
  at a time, the most important first. They are redrawn when the map stops
  moving.

Borders and Cities are offered only when `overlays/meta.json` exists in the
map directory (`paths.map_dir`, else `<prefix>/share/skycue/www/`) (the page asks for it once when it loads; without it the browser's
console shows that one request as a 404 and the two checkboxes are left out).
Make the overlays on a connected machine, or on any machine whose cache
already holds the three Natural Earth downloads:

```sh
scripts/maps/make-overlays.sh                                   # needs ogr2ogr (GDAL) on PATH
scripts/maps/make-overlays.sh --container ghcr.io/osgeo/gdal:ubuntu-small-3.9.3   # ogr2ogr in podman/docker
scripts/maps/make-overlays.sh --dry-run                         # print the commands only
cp -r www/overlays <prefix>/share/skycue/www/    # or paths.map_dir, or ./www/ for a build-tree run
```

`make-overlays.sh [--cache DIR] [--out DIR] [--container IMAGE] [--dry-run]`
downloads `ne_50m_admin_0_boundary_lines_land`,
`ne_50m_admin_1_states_provinces_lines` and `ne_10m_populated_places_simple`
(about 1.2 MB of zip files, public domain, each pinned by SHA-256) into the
cache (default `build/naturalearth/` when run by hand) once, and writes `admin0.json`,
`admin1.json` and `cities.json` (compact GeoJSON, lines simplified to about
1 km; about 1 MB in all) and `meta.json` (each file's source, version,
SHA-256, feature count and size, and when it was made) to the output
directory (default `www/overlays/`, git-ignored).
`SKYCUE_NE_VECTOR_URL` points it at a mirror of the same zip files.
skycued serves the directory like the rest of the page, so a reload picks it
up without a restart.

## Watching the stream: skycue-dump

`skycue-dump` prints a skycue stream one line per frame (`HELLO`, `UPDATE`,
`DROP`, `HB`, or `BAD` for a frame that does not decode), plus event lines
(`CONN up|down`, `LOCAL-DROP` for a drop the client inferred itself):

```sh
skycue-dump                              # connect to 127.0.0.1:30500 (cues.tcp_port)
skycue-dump --tcp radar-pc:30500 --json  # one JSON object per line
skycue-dump --udp 0.0.0.0:40000          # receive what cues.udp_targets sends here
skycue-dump --tcp-listen 0.0.0.0:40001   # wait for skycued to dial in (cues.tcp_connect)
skycue-dump --count 20 --timeout 30      # stop after 20 frames or 30 s
```

`--timeout` exits 0 if any frame arrived and 3 if none did, which makes it a
quick health check. With `--tcp-listen` any number of skycued instances may
connect. `--json` strings are UTF-8: valid UTF-8 passes through as it is, and
a byte that is not valid UTF-8 is written as U+FFFD. This changed in the
hardening sweep: `skycue-dump` used to pass such bytes through (as `\u00XX`
escapes); it now replaces invalid UTF-8 with U+FFFD, as every other JSON
writer in skycue does.

`--capture FILE` prints a pingStation capture file instead and exits: one
`<t> <input> <transport> <n> bytes` line per record, then the text of a JSON
datagram, or one line per CAT021 record (`--edition`, 2.2 to 2.7, default
2.5) with the fields that are present:

```
2026-11-02T10:15:00.123Z sim-cat udp 51 bytes
  CAT021 sac=0 sic=0 addr=AC5E81 lat=37.88798 lon=-76.26140 fl=180.00 squawk=4711 ident=RPA4521 gnd=-
```

`gnd` is `Y` (on the ground) or `-` when I021/040 says, and absent when it
does not. A block that does not decode prints `  BAD <reason>` after the
records decoded before the error. `--capture` excludes the stream options.

## Analysing recordings: skycue-stats

`skycue-stats` summarises recordings to check a receiver's coverage and the
filter / rate-limit chain. It reads received-stream and sent-stream JSONL
recordings and captures of readsb's JSON port, plain or `.gz`, recognising
each line on its own; several files are merged in time order.

```sh
skycue-stats --config skycue.toml /data/rec/received-20261101-120000.jsonl
skycue-stats --lat 38.9 --lon -77.0 capture.jsonl.gz
skycue-stats --config skycue.toml --json received-*.jsonl sent-*.jsonl > stats.json
```

| option | default | meaning |
| --- | --- | --- |
| `--lat L --lon L` | | receiver position (overrides `--config`) |
| `--config FILE` | | take `receiver.lat`/`receiver.lon` (and `receiver.alt_m` + `receiver.antenna_height_m` for elevation) from a skycue.toml |
| `--json` | | one JSON object (same sections, keys below) instead of text |
| `--top N` | 15 | rows in the per-aircraft tables |
| `--range-bin NM` | 10 | range histogram bin |
| `--alt-bin FT` | 5000 | altitude histogram bin |
| `--gap-s S` | 5 | report stretches longer than this without any record |

Without a receiver position the range, elevation and azimuth sections print
`(skipped: receiver position unknown; pass --lat/--lon or --config)` (`null`
in JSON). Exit status: 0, 1 for a file that cannot be read or a bad
`--config`, 2 for a usage error. Lines that are not records are skipped with
one warning per file on stderr.

The sections (JSON keys in brackets):

1. **Overview** (`overview`): each file with its kind (`received`, `sent`,
   `readsb-raw`) and record count; first/last record, duration, records by
   type/event, records per second (mean, and the most in any one second).
2. **Aircraft** (`aircraft`): distinct aircraft (address + address type), by
   source and address type, on-ground/airborne observations, how many have a
   callsign or registration, and every 7500/7600/7700 squawk.
3. **Range histogram** (`range_histogram`): great-circle distance from the
   receiver; observations and distinct aircraft per bin, maximum and median.
4. **Altitude histogram** (`altitude_histogram`): barometric, else geometric.
5. **Elevation histogram** (`elevation_histogram`): 5° bins, as skycued's
   `filters.min_elevation_deg` filter computes it, plus the count below 0°.
6. **Coverage by azimuth** (`azimuth`): twelve 30° sectors by bearing from the
   receiver (0° = north): observations, aircraft and maximum range, as a table
   and as a bar chart of the maximum range.
7. **Speed and vertical rate** (`speed`): ground speed min/median/max;
   climbing/descending/level shares (level: within ±100 ft/min).
8. **Gaps** (`gaps`): stretches with no record longer than `--gap-s`, e.g. a
   readsb or radio outage.
9. **Top N aircraft** (`top_aircraft`): by observations.
10. **Sent stream** (`sent`, only with update/drop/filter_change records):
    updates, keyframe events (always 0: skycued writes every sent track as an
    `update`), drops by reason, the `filter_change` records, updates per
    aircraft, and, given a received stream as well, the compression ratio
    (received observations ÷ updates) and the received aircraft never sent.
11. **FIS-B products by ID** (`fisb`, only with `uat_uplink` records; `null`
    in JSON otherwise): the uplinks run through skycued's FIS-B decoder and
    store on the recording's clock: uplinks, APDUs and products decoded;
    products by ID; product-413 text records by kind (decoded, and held at
    the end); TWGO halves and records by product; the mosaics held at the
    end with their block counts; the decoder and store counters. The counts
    are the ones `skycue-fisb --summary` prints for the same file
    ([fisb.md](fisb.md#skycue-fisb)).

Histograms and the per-aircraft sections count *observations*: the received
stream's records when one is given, otherwise the sent stream's updates.
Recording during a replay keeps the replayed times in the received stream
but wall-clock times in the sent stream, so the overview and gaps of such a
pair span both periods. Hex addresses of non-ICAO address types carry
readsb's leading `~`. Bars are scaled so that the longest is 40 characters.

Sample (the test fixture, abridged):

```
$ skycue-stats --lat 38.9 --lon -77.0 readsb-sample.jsonl
skycue-stats report

== Overview ==
Files:
  readsb-sample.jsonl  readsb-raw  1394 records
Stream kinds: readsb-raw
First record: 2026-11-01T15:00:00.052Z
Last record:  2026-11-01T15:02:01.700Z
Duration: 121.6 s
Records: 1394
  obs           1394
Records per second: mean 11.46, peak 12
Receiver: 38.900000, -77.000000 (from --lat/--lon); antenna altitude 0.0 m
Observations: received-stream obs records

== Aircraft ==
Aircraft: 12
Observations: 1394 (airborne 1272, on ground 122)
...
Emergency squawks (7500/7600/7700): none

== Range histogram (10 nm bins) ==
Positions: 1363 obs; max 158.9 nm (a8c3e5 at 2026-11-01T15:02:00.251Z); median 23.3 nm
     0-10 nm | 291 obs | 4 ac | #####################################
    10-20 nm | 314 obs | 3 ac | ########################################
    20-30 nm | 122 obs | 2 ac | ################
...
  150-160 nm | 115 obs | 2 ac | ###############

== Coverage by azimuth (30 deg sectors) ==
  sector       obs aircraft   max nm
  000-030      244        3    158.9
  030-060      120        1      8.0
  060-090        0        0        -
...
Max range by sector:
  000-030 |  158.9 nm | ########################################
  030-060 |    8.0 nm | ##
...

== Sent stream ==
(no sent-stream records)
```

With a received and a sent recording of the same run, the last section reads
like this (abridged):

```
== Sent stream ==
Updates: 28
Keyframe events: 0 (as expected: skycued records every sent track as an update event)
Updates with the keyframe flag: 0
Drops by reason:
  timeout             6
  filtered            2
  source_lost         8
  shutdown            0
  stale               0
Filter changes: 1
  2026-11-02T01:32:38.087Z  startup: receiver=38.9,-77 alt_m=0 max_range_nm=150 ...
...
Compression: 1394 received obs / 28 updates = 49.79
Received aircraft never sent: 1
  a7d3e6
```

On a recording with FIS-B uplinks (the 2015 test fixture,
`received-with-uplinks.jsonl`, abridged):

```
== FIS-B products by ID ==
Uplinks: 704, APDUs: 563, products decoded: 561
Products by ID (a segmented set counts once):
      8  NOTAM                  62
     11  AIRMET                  2
     12  SIGMET                  2
     13  SUA                    71
     63  NEXRAD regional       200
    413  generic text          224
Text records (product 413):
  metar  decoded    150  stored    127
  taf    decoded     33  stored     33
  pirep  decoded      6  stored      2
  winds  decoded     35  stored     35
TWGO records by product (decoded halves; records held at the end):
      8  NOTAM            27 text, 6 keep-alive, 29 graphics, 0 cancel; stored 23 (0 cancelled)
...
Mosaics (held at the end):
  63     NEXRAD regional  2015-01-24T04:10:00.000Z  671 blocks, published 1
Decoder counters:
  bad_hex                      0
  bad_frames                   0
...
```

## FIS-B weather

With `fisb.enable = true` (the default) skycued decodes the FIS-B ground
uplinks that dump978-fa passes it (or that a replayed recording holds):
NEXRAD radar and the other weather rasters, METAR/TAF/PIREP/winds text,
and NOTAM/TFR, AIRMET, SIGMET and SUA reports with their shapes. It keeps
the current products until they expire, records them as `fisb-*.jsonl`
when `recording.formats` includes `jsonl`, serves them under `/api/fisb/`, and
draws them on the web map (radar on by default; NOTAM / TFR, SIGMET,
AIRMET, CWA, SUA and the other rasters in the Layers box) and in the
**Weather** tab of the side panel. `skycue-fisb` decodes an uplink file
offline. What is decoded and what is not, the store's rules, the CLI and
the limits: [fisb.md](fisb.md). FIS-B is for situational awareness only,
never for flight decisions.

### Report locations: fisb-locations.json

METARs, TAFs, winds and PIREPs name a location id (`KPRG`) but carry no
coordinates. skycued learns positions from the NOTAM and TFR shapes it
decodes (airport-style ids only: `K???`, `C???`, `PA??`, `PH??`) and keeps
them in `fisb-locations.json` in `paths.state_dir` when that is set, else next
to `skycue.toml`: written with an atomic
rename at most once a minute when something new was learned, and at
shutdown; read at startup, so coverage builds up across restarts. A
failed write is logged once at WARN and retried. A file that does not parse
is renamed `fisb-locations.json.rejected`, the reason is logged at ERROR,
and skycued starts with no learned locations. One it cannot read (its
permissions, say) is renamed `fisb-locations.json.unreadable-<UTC time>`
first, so the next save does not overwrite it; if that rename fails, or the
name is a directory or other non-file, it is left alone and nothing is
saved until the next start. Each of these logs one ERROR line. Deleting the
file (with skycued stopped) forgets them. At most 20,000 learned ids are
kept; new ones past that are ignored with one WARN (a file holding more is
rejected as above). As with `skycue.runtime.json`, every skycued whose
`skycue.toml` is in the same directory (or that has the same `paths.state_dir`)
shares this one file, so a replay
run with the live configuration adds the positions it learns to the live
file (they are real positions, so this is harmless; give a replay its own
configuration directory, or its own `paths.state_dir`, to keep them apart).

`paths.locations_file` optionally names a CSV of your own (a relative path is
taken from `skycue.toml`'s directory; `id,lat,lon` per line;
`#` comments, blank lines and an `id,lat,lon` header are skipped), merged
under the learned positions: a learned position wins for the same id, and
the CSV rows are never written to `fisb-locations.json`. Lines that do not
parse are skipped with one WARN giving the count and the first line number;
a file that cannot be read stops skycued with exit status 2. At most
250,000 ids are read from it; the rest are ignored with one WARN giving
the count (a world-wide `make-locations.py` file is well under that, and
the script warns when its output is not).
`scripts/maps/make-locations.py` builds one from OurAirports' public-domain
data ([docs/make-locations.md](make-locations.md)); the
result is a local file, never committed. The merged table (learned and CSV) is served as
`GET /api/fisb/locations` ([web-api.md](web-api.md#get-apifisblocations)).

## The consumer library

Programs that act on cues link `libskycue` (`skycue::skycue`), whose
`skycue::Client` receives the stream in any of skycued's three modes. Build
integration, the API, threading, drops and liveness: [libskycue.md](libskycue.md).

## For developers: sanitizers, fuzzing, static analysis, soak

None of this is needed to run skycue, and none of it is in the default build
or any release. Before changing a parser, the daemon's threading or
anything that runs for days, run the checks in
[hardening.md](hardening.md): `scripts/dev/run-sanitizers.sh asan|tsan`,
`scripts/dev/run-fuzz.sh`, `scripts/dev/run-static.sh` and
`tests/soak/soak.sh`, with their options, skip rules and how to add a
fuzz harness for a new parser; the hardening notes also list what each part
found and fixed.
