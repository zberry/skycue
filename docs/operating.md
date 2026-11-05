# Operating skycue

How to run the receiver chain from an installed prefix (`/opt/skycue` below;
`build/stage` works the same), replay recordings, and watch the cue stream.
Building and installing the offline bundle are in
[building-rhel.md](building-rhel.md); the Debian and RHEL packages and the
plain CMake install, which run skycued alone beside decoders you already
have (as the systemd unit `skycued`, with `/etc/skycue/skycue.conf`), are in
[installing.md](installing.md). Where the two shapes keep their files
differently, [Where the files are](#where-the-files-are) says so.

## The chain

```
RTL-SDR 978 MHz -> dump978-fa --raw--> readsb <- RTL-SDR 1090 MHz
                       |                 |
                       | raw UAT         | JSON (READSB_JSON_PORT)
                       v                 v
                       +---------> skycued ---> cue clients (TCP / UDP)
                                         \---> RECORD_DIR (received, sent)
```

`dump978-fa` decodes 978 MHz UAT and serves raw frames; `readsb` decodes
1090 MHz, merges the UAT traffic it reads from dump978-fa, and serves one
JSON line per aircraft update. `skycued` reads that JSON, keeps a track
table, filters it (range, elevation, curated lists), decides when each
aircraft is worth a new cue, and forwards cues in the skycue wire protocol
([protocol.md](protocol.md)). It also reads dump978-fa's raw port, but only
to count UAT uplinks.

## Start, stop, status

```sh
cd /opt/skycue
cp etc/skycue/skycue.conf.example etc/skycue/skycue.conf
# edit: RECEIVER_LAT, RECEIVER_LON, SDR_1090_SERIAL, SDR_978_SERIAL (either may be empty)
bin/skycue-start.sh             # dump978-fa, readsb, skycued (+ skycue-rawrec)
bin/skycue-start.sh status      # each process: running/stopped, pid, uptime, last log lines
bin/skycue-stop.sh              # everything, in reverse order (= skycue-start.sh stop)
```

`start` (and `replay`, `replay-raw`, `replay-iq`) ends with the address of the web UI,
`web UI: http://<WEB_BIND>:<WEB_PORT>/` (none with `WEB_PORT=0`); `status`
shows the same line under skycued while it runs.

`start` launches the processes in order and waits for each to be ready
before the next: dump978-fa until its raw port accepts, readsb until its JSON
port accepts, skycued until `CUE_TCP_PORT` accepts (with `CUE_TCP_PORT=0`
there is no listener, so it only checks that skycued stays up). Before
launching anything it refuses to start if one of those ports is already
taken (only the ports of the processes it is about to launch; see
[Running without SDR dongles](#running-without-sdr-dongles)). If any step fails, what was started is stopped again; the reason is
in `<prefix>/var/run/<name>.log`.

Options, for every subcommand:

- `--dry-run`: validate the config and print the environment and the
  `would run:` command lines (three with both dongles; one more with
  `RECORD_RAW=yes`) and the
  `web UI:` line, starting nothing and creating no files.
- `-c CONFIG`: use another config file (default
  `<prefix>/etc/skycue/skycue.conf`).

Pid and log files live in `<prefix>/var/run`, or in `$SKYCUE_RUN_DIR` when
that is set (it is not a config key, so `stop` and `status` work without a
config). skycued gets the config file itself (`skycued --config FILE`) and
reads its own keys from it.

### Testing the decoder connections: skycued --check-inputs

```sh
skycued --config skycue.conf --check-inputs
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
file), dump978 (when `DUMP978_ENABLE` is on and not `--no-uat`) and every
enabled `tcp_connect` input of the inputs file, using `READSB_HOST`,
`DUMP978_HOST` and the ports from the config (`--readsb-host` and
`--dump978-host` win). Enabled `udp` and `tcp_listen` inputs wait for data
and are listed as not checked. The exit status is 0 when everything checked
connected, 1 when any failed, 2 for a usage or config error (including
`--replay` together with `--check-inputs`). Nothing else starts: no
listeners, no recordings, no state files.

Each check gives up after 3 s, and all run at once. A host name lookup
cannot be cancelled, so one that is still running at that point is reported
as `timed out` and abandoned; it ends on the system resolver's own timeout,
after skycued has exited.

### Running without SDR dongles

`SDR_1090_SERIAL` and `SDR_978_SERIAL` may be left empty; `start` launches a
decoder only for a dongle whose serial is set, and tells skycued not to
connect to the decoder it leaves out. When both are set they must differ.

| Serials set | `start` launches | skycued's inputs |
|-------------|------------------|------------------|
| both | dump978-fa, readsb, skycued | readsb (1090 and UAT aircraft), dump978 (uplinks), plus the inputs file |
| `SDR_1090_SERIAL` only | readsb, skycued `--no-uat` | readsb (1090 aircraft), plus the inputs file |
| `SDR_978_SERIAL` only | dump978-fa, skycued `--no-readsb` | dump978 (uplinks and FIS-B only), plus the inputs file |
| neither | skycued `--no-readsb --no-uat` | the inputs file only |

With only the 978 dongle skycued counts and decodes the uplinks but shows no
UAT aircraft: those reach it through readsb, which merges dump978-fa's
frames and needs the 1090 dongle. A machine whose only receiver is a
pingStation 3 leaves both serials empty and defines the pingStation in
[the inputs file](#the-inputs-file); `start` then runs skycued alone, and the
web UI or the API add and change inputs while it runs.

`RECORD_RAW=yes` launches skycue-rawrec only with a decoder, recording only
the streams launched (`--beast` for readsb, `--uat` for dump978-fa). With
neither there is no skycue-rawrec; skycued still writes the raw frames of its
pingStation inputs to `RECORD_DIR`. The port checks cover only the processes
being launched, and `status` and `stop` list every process as before (the
ones not launched show as `stopped`).

## Config keys that matter

The full list, with comments and the file grammar, is
`etc/skycue/skycue.conf.example`. An unknown key or a malformed line stops
both skycue-start.sh and skycued with exit status 2. An unexpected error
that escapes `skycued`, `skycue-rawrec` or `skycue-pingsim` ends it with
exit status 1 and one line, `fatal: <what>` at ERROR in skycued's log or
`skycue-rawrec: <what>` / `skycue-pingsim: <what>` on stderr.

| Key | Used by | Meaning |
|-----|---------|---------|
| `RECEIVER_LAT`, `RECEIVER_LON` | all | receiver position (required) |
| `RECEIVER_ALT_M`, `ANTENNA_HEIGHT_M` | skycued | antenna altitude for elevation angles |
| `SDR_1090_SERIAL`, `SDR_978_SERIAL` | start | the two dongles (each optional, see [Running without SDR dongles](#running-without-sdr-dongles); must differ when both are set) |
| `GAIN_1090`, `GAIN_978` | start | `auto` or a gain in dB |
| `READSB_JSON_PORT`, `DUMP978_RAW_PORT` | start, skycued | where skycued reads from |
| `READSB_HOST` | skycued | host of the readsb JSON port (default `127.0.0.1`, not empty); skycue-start.sh runs readsb on this machine and passes `--readsb-host 127.0.0.1`, which overrides it |
| `DUMP978_HOST` | skycued | host of the dump978-fa raw port (default `127.0.0.1`, not empty); skycue-start.sh passes `--dump978-host 127.0.0.1`, which overrides it |
| `DUMP978_ENABLE` | skycued | `no`: there is no 978 MHz decoder, skycued neither connects to one nor expects UAT input (as `--no-uat`, which overrides it); default `yes` |
| `STATE_DIR` | skycued | absolute directory for skycued's files; a relative `INPUTS_FILE`, `LOCATIONS_FILE`, `STATS_FILE`, `ALLOW_LIST_FILE` or `BLOCK_LIST_FILE` is taken from it, and it holds the runtime overlay `skycue.runtime.json` and the `skycue.overrides.conf` that save as baseline writes instead of rewriting `skycue.conf` (see [web-api.md](web-api.md#save-as-baseline)); empty (default): today's places (next to `skycue.conf`; the list files from the working directory) |
| `MAP_DIR` | skycued | absolute directory the web map's `tiles/` and `overlays/` are served from, and nowhere else; empty (default): the web directory's |
| `CUE_TCP_PORT` | skycued | cue clients connect here; `0`: no listener |
| `CUE_TCP_CONNECT` | skycued | `host:port ...` of clients that listen; skycued dials them |
| `CUE_UDP_TARGETS` | skycued | `host:port ...` to send UDP cues to |
| `MAX_RANGE_NM`, `MIN_ELEVATION_DEG` | skycued | aircraft farther or lower are not sent |
| `ALLOW_LIST_FILE`, `BLOCK_LIST_FILE` | skycued | curated lists (below) |
| `MIN_INTERVAL_S`, `MAX_INTERVAL_S` | skycued | per-aircraft cue rate: at most one per min, at least one per max |
| `TRACK_TIMEOUT_S` | skycued | silence before a track is dropped |
| `KEYFRAME_INTERVAL_S` | skycued | full-state keyframes on the stream |
| `RECORD_DIR`, `RECORD_FORMATS` | skycued | where and how to record (below) |
| `RECORD_RAW` | start, skycued | also record the raw receiver streams; skycued also writes pingStation JSON `rawData` frames |
| `RECORD_MAX_GB` | skycued, start | disk budget for the recordings (start passes it to skycue-rawrec); `0`: unlimited |
| `WEB_PORT`, `WEB_BIND` | skycued | the web UI (below); `WEB_PORT=0`: off |
| `TRAIL_SECONDS` | skycued | trail length on the web map |
| `FISB_ENABLE`, `FISB_EXPERIMENTAL` | skycued | decode FIS-B weather from the 978 MHz uplinks (default yes); also decode the unverified layouts (default no). See [FIS-B weather](#fis-b-weather) |
| `LOCATIONS_FILE` | skycued | optional CSV `id,lat,lon` of report locations, merged under the learned ones; a relative path is taken from `skycue.conf`'s directory, or from `STATE_DIR` when it is set; read at startup (a change needs a restart). See [Report locations](#report-locations-fisb-locationsjson) |
| `STATS_FILE` | skycued | where the statistics history is kept (default `skycue.stats.json` in `STATE_DIR`, or next to `skycue.conf` when that is empty); a relative path is taken from `skycue.conf`'s directory, or from `STATE_DIR` when it is set; empty: kept in memory only; a file that cannot be read back is renamed `<file>.bad`; read at startup (a change needs a restart). See [Statistics file](#statistics-file) |
| `STATS_AIRCRAFT_MAX` | skycued | aircraft kept in the statistics ledger, a whole number 100..100000 (default 20000); past it the 5 % heard longest ago are forgotten; a change needs a restart |
| `STATS_SAVE_S` | skycued | seconds between saves of `STATS_FILE` while it changed, a whole number 10..86400 (default 300); also saved at a clean shutdown and at once on a reset; a change needs a restart |

The three forwarding modes (`CUE_TCP_PORT`, `CUE_TCP_CONNECT`,
`CUE_UDP_TARGETS`) can be used together; with all three off skycued runs but
warns that it forwards to nobody.

### Where the files are

The bundle (and a `skycue.conf` copied from the example) leaves `STATE_DIR`
and `MAP_DIR` empty, so skycued keeps its files next to `skycue.conf` as it
always has. The packaged config sets them, and an install from source sets
them by hand ([installing.md](installing.md#from-source)):

| File | Bundle (`STATE_DIR`, `MAP_DIR` empty) | Package (`STATE_DIR=/var/lib/skycue`, `MAP_DIR=/var/lib/skycue/maps`) |
|------|------|------|
| config | `<prefix>/etc/skycue/skycue.conf` | `/etc/skycue/skycue.conf` (`/usr/local/etc/skycue/skycue.conf` from source); skycued never writes it |
| inputs, statistics, learned locations (`skycue.inputs.json`, `skycue.stats.json`, `fisb-locations.json`) | next to `skycue.conf` | in `STATE_DIR` |
| runtime overlay (`skycue.runtime.json`) | next to `skycue.conf` | in `STATE_DIR` |
| what **Save as baseline** writes | `MAX_RANGE_NM`/`MIN_ELEVATION_DEG` in `skycue.conf`, and the list files | `STATE_DIR/skycue.overrides.conf`, and the list files |
| allow and block lists | as `ALLOW_LIST_FILE`/`BLOCK_LIST_FILE` say (relative: skycued's working directory) | `allow.list`, `block.list` in `STATE_DIR` |
| map tiles and overlays | `<prefix>/share/skycue/www/tiles`, `overlays` | `MAP_DIR/tiles`, `MAP_DIR/overlays` |
| recordings | `RECORD_DIR` | `RECORD_DIR` (commented out; `/var/lib/skycue/recordings` is ready for it) |
| log | `<prefix>/var/run/skycued.log` | the journal (`journalctl -u skycued`) |

`skycue.overrides.conf` is in the `skycue.conf` grammar and holds only
the keys the page may save (`MAX_RANGE_NM`, `MIN_ELEVATION_DEG`,
`ALLOW_LIST_FILE`, `BLOCK_LIST_FILE`); skycued reads it after
`skycue.conf`, so its values win. It exists only when `STATE_DIR` is set:
the packaged config is a file the package manager owns, so the daemon,
which runs as the `skycue` user with only `/var/lib/skycue` writable,
never rewrites it. To make a saved value part of `skycue.conf` again, copy
it there and delete the line from `skycue.overrides.conf`.

## Replaying a JSONL recording

```sh
bin/skycue-start.sh replay /data/rec/received-20261101-120000.jsonl            # real time
bin/skycue-start.sh replay /data/rec/received-20261101-120000.jsonl --speed 10 # 10x
bin/skycue-start.sh replay FILE --speed 0 --loop  # as fast as possible, over and over
bin/skycue-stop.sh
```

`replay FILE` runs skycued alone (`skycued --replay FILE --replay-speed N
[--replay-loop] --config ...`): no SDRs, no dump978-fa, no readsb. FILE is a
received-stream recording written by skycued, or a capture of readsb's JSON
port (one JSON object per line). Everything downstream is as in a live run:
the same filters, the same forwarding, and recording too if `RECORD_DIR` is
set. `--speed 1` (the default) keeps the recorded timing; `--speed 0` sends
as fast as skycued can. The SDR keys are not needed, and a replay refuses to
start while any part of the live chain runs.

## Raw capture and replay-raw

With `RECORD_RAW=yes` (and `RECORD_DIR` set), `start` also runs
`skycue-rawrec`, which records what the decoders produce before any
interpretation: readsb's Beast output as `RECORD_DIR/raw/beast-*.bin` and
dump978-fa's raw UAT lines as `RECORD_DIR/raw/uat-*.txt`, a new file every
256 MB or 60 minutes. These let a session be decoded again later, by a newer
readsb for instance.

```sh
bin/skycue-start.sh replay-raw /data/rec/raw/beast-20261101-120000.bin \
    /data/rec/raw/uat-20261101-120000.txt --speed 1
```

`replay-raw FILE...` runs a network-only readsb (no device) on the usual
`READSB_JSON_PORT`, then skycued reading it as it would live, then
`skycue-rawplay` feeding the files into readsb. Accepted files: `.bin`
(Beast), `.csv[.gz]` (`epoch_seconds,HEX` Mode S lines) and `.txt[.gz]`/`.log`
(dump978 raw UAT lines). `--speed N` and `--loop` work as for `replay`. In
this mode skycued does not open the UAT port (rawplay serves it to readsb
only), so uplinks are not counted. The receiver position in the config must
match the capture's region, or readsb cannot decode positions and the range
filter drops everything: the bundled sample capture is European, so use
`RECEIVER_LAT=52.0` / `RECEIVER_LON=4.5` for it.

## IQ files: replay-iq

```sh
bin/skycue-start.sh replay-iq /data/iq/sample.1090.cu8 /data/iq/sample.978.cu8
bin/skycue-start.sh replay-iq /data/iq/capture.cu8 --band 978 --loop
bin/skycue-stop.sh
```

`replay-iq FILE...` puts recorded IQ samples (8-bit unsigned I/Q, `cu8`)
where the SDRs would be, so the decoders themselves run on them: dump978-fa
with `--file FILE --format CU8 --file-throttle` on the usual
`DUMP978_RAW_PORT`/`DUMP978_JSON_PORT`, readsb with `--device-type ifile
--ifile FILE --iformat UC8 --throttle` on the usual ports (merging
dump978-fa's UAT as in `start`), and skycued as in `start`. Give at most one
file per band. The band comes from the name (`*.1090.cu8`, `*.978.cu8`);
`--band 1090|978` sets it for a single file named otherwise. With only a
1090 file there is no dump978-fa and skycued runs with `--no-uat`; with only
a 978 file readsb runs network-only, merging dump978-fa's UAT, so skycued
sees it as it would live. The SDR keys are not needed, and like the other
replays it refuses to start while any part of the live chain runs.

Playback is real time only (the decoders' own throttling; there is no
`--speed`). Each decoder exits at the end of its file; with `--loop` it is
started again (by an internal `skycue-start.sh _respawn` wrapper, which
`status` and `stop` see as the decoder) while skycued keeps running, and
`stop` stops the wrapper and the decoder together (a decoder that ignores
TERM is killed after 3 s). With only a 978 file, `--loop` restarts
dump978-fa only: readsb runs network-only and has no end of file. As with `replay-raw`,
the receiver position in the config must match the capture, or readsb
cannot decode positions and the range filter drops everything: for files
made from the bundled Mode S capture use `RECEIVER_LAT=52.0` /
`RECEIVER_LON=4.5`; for the Porto capture fetched by
`scripts/fetch-iq-samples.sh` use `RECEIVER_LAT=41.15` /
`RECEIVER_LON=-8.61`. Making IQ files (`skycue-iqgen`), the real capture and
what the decoders recovered from both are described in
[iq-replay.md](iq-replay.md).

## The inputs file

skycued's inputs are the two built-ins, `readsb` and `dump978` (the decoders
`skycue-start.sh` launches; a built-in whose decoder is not launched is
absent, see [Running without SDR dongles](#running-without-sdr-dongles)), plus any inputs you add, such as a pingStation 3.
The added inputs and the enabled state of the built-ins are kept in
`skycue.inputs.json` in `STATE_DIR` when that is set (`/var/lib/skycue` in
the packages), else next to `skycue.conf`. It is the only place they are
stored: a change made in the web UI or the API is written there at once
(with an atomic rename), and there is nothing to "save" separately. Until the
first change there is no file, and skycued runs the built-ins only. The
API's `/api/inputs` routes make the same changes and report each input's
state and counters; they and every definition key are described in
[web-api.md](web-api.md#inputs).

```json
{"version":1,
 "inputs":[
  {"id":"ps-north","name":"North mast","type":"pingstation_cat021","enabled":true,"transport":"udp","bind":"0.0.0.0","port":4001,"peer":null,"edition":"2.5","sac":null,"sic":null,"status_url":null,"status_interval_s":30,"time_mode":"utc","capture":false}
 ],
 "builtin":{"readsb":{"enabled":true},"dump978":{"enabled":true}}}
```

`version` must be 1. `inputs` and `builtin` may be left out; a built-in
missing from `builtin` keeps its default (enabled). Each entry follows the
definition rules of the API (an unknown key, a key that does not apply to
the type, or a value out of range is an error), and two enabled inputs may
not listen on the same port and protocol. `"capture":true` needs `RECORD_DIR`:
it writes every datagram (or TCP block) the input receives to
`RECORD_DIR/capture-<id>-*.jsonl`, one JSON line each. Without `RECORD_DIR`
the web UI and the API refuse it (400 "capture needs RECORD_DIR"); in the
file, at startup or in a hand edit, the entry is kept and runs, skycued logs
one WARN ("capture needs RECORD_DIR: not capturing") and nothing is captured. With `RECORD_RAW=yes` a `pingstation_json` input also writes the raw 1090 and
UAT frames the device includes (`rawData`) to `RECORD_DIR/raw-<id>-*.csv` and
`.txt`, which `skycue-rawplay` and `replay-raw` accept like skycue-rawrec's
files. Both formats are in [recording-format.md](recording-format.md#pingstation-capture-and-raw-frames).

**Hand edits.** skycued checks the file about once a second and applies a
change once the file has stopped changing for a second, so an editor that
saves in several steps is never read half written. The change is applied by
id: a new id is started, a removed one stopped, a changed definition restarts
that input (its counters carry on), and inputs you did not touch keep
running undisturbed.

**A file that is not valid** (bad JSON, a bad entry, a port clash; not a
`"capture":true` without `RECORD_DIR`, which is only warned about) is
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
It is how the Receiver tab is demonstrated and how the end-to-end test runs.
It reads one of two kinds of file:

- `--capture FILE`: a capture file written by an input with
  `"capture": true` (`capture-<id>-*.jsonl`, see
  [recording-format.md](recording-format.md#pingstation-capture-and-raw-frames)).
  Every record's bytes go out exactly as they were received, paced by the
  record's `t`.
- `--as json FILE` or `--as cat021 FILE`: a received-stream recording
  (`received-*.jsonl`) or a capture of readsb's JSON port, such as
  `skycue/tests/fixtures/readsb-sample.jsonl`. Each observation is
  synthesised as a pingStation JSON aircraft object, or as a one-record
  CAT021 edition 2.5 block, stamped with the time it is sent and paced by the
  observation times. Sent-stream records and uplinks are skipped.

FILE may be gzip-compressed. pingsim sends to one of:

- `--udp HOST:PORT`: one datagram per record, for a `pingstation_json` input
  or a `udp` `pingstation_cat021` input.
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

To demonstrate the Receiver tab with no readsb or dump978 running, put two
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
bin/skycued --no-readsb --no-uat --inputs-file sim.inputs.json --config etc/skycue/skycue.conf &

# JSON with a device status every 10 s: the card shows the device line
# (GPS 3D fix, receiver normal, firmware 1.0.0) and a device marker there.
bin/skycue-pingsim --udp 127.0.0.1:4000 --as json --loop \
    --status-interval 10 --status-lat 38.9 --status-lon -77.0 \
    skycue/tests/fixtures/readsb-sample.jsonl &
# CAT021 from the same recording, five times faster.
bin/skycue-pingsim --udp 127.0.0.1:4001 --as cat021 --speed 5 --loop \
    skycue/tests/fixtures/readsb-sample.jsonl
```

The fixture's aircraft are around 38.9 N, 77.0 W, so `RECEIVER_LAT` and
`RECEIVER_LON` must be near there or the range filter drops them. The status
message carries guid `0000000000000001`, version 1.0.0, `gpsStatus` 3,
`receiverStatus` 0, the send time and the given position; `--status-interval`
needs `--as json` and both `--status-lat` and `--status-lon`.

**Replaying a capture.** Add `"capture":true` to the `sim-cat` entry above
(or a real device's input) and set `RECORD_DIR` in skycue.conf (the example
config leaves it empty, and capture needs it): the input then writes
`RECORD_DIR/capture-sim-cat-*.jsonl`. Play it back into the same kind of
input, and look inside it with `skycue-dump --capture`
([below](#watching-the-stream-skycue-dump)):

```sh
bin/skycue-pingsim --udp 127.0.0.1:4001 --capture /data/rec/capture-sim-cat-20261102-101500.jsonl
bin/skycue-dump --capture /data/rec/capture-sim-cat-20261102-101500.jsonl | less
```

A capture holds the device's own timestamps from when it was recorded:
replayed later they are outside the input's ±10 s clock window, so the input
counts them in `clock_rejects` and dates the positions by arrival instead.
Synthesised records (`--as`) carry the send time and are accepted.

## Recordings

With `RECORD_DIR` set, skycued writes these streams there. The first two are
in the formats listed in `RECORD_FORMATS` (`jsonl`, `csv`, or both):

- `received-YYYYMMDD-HHMMSS.{jsonl,csv}`: every observation from readsb and
  the pingStation inputs, and every UAT uplink, before filtering. The JSONL file is what `replay FILE` plays back.
- `sent-YYYYMMDD-HHMMSS.{jsonl,csv}`: every update and drop skycued
  forwarded, and the active filters at startup.
- `fisb-YYYYMMDD-HHMMSS.jsonl` (JSONL only, with `FISB_ENABLE=yes`): the
  decoded FIS-B weather.
- Per pingStation input, also directly under `RECORD_DIR`:
  `capture-<id>-*.jsonl` for an input with `"capture":true`, and, with
  `RECORD_RAW=yes`, `raw-<id>-*.csv` (1090) and `raw-<id>-*.txt` (UAT) for a
  `pingstation_json` input ([the inputs file](#the-inputs-file)). Each of
  these is a stream of its own.

skycue-rawrec's raw captures (`RECORD_RAW=yes`, readsb's Beast output and
dump978's raw lines) go under `RECORD_DIR/raw` ([above](#raw-capture-and-replay-raw)).
Every file rotates at 256 MiB or after an hour; the stamp is the UTC time a
file was opened. The record layouts are in
[recording-format.md](recording-format.md).

### Disk planning

Set `RECORD_MAX_GB` to the disk space the recordings may use (say 80% of
the file system's free space): skycued and skycue-rawrec then delete the
oldest recordings under `RECORD_DIR` and `RECORD_DIR/raw` whenever they start
a new file, keeping the total within the budget (details in
[recording-format.md](recording-format.md#retention-record_max_gb)). Only
files with skycue's own names are counted or deleted, so a copy you want to
keep is safe once renamed or moved elsewhere. With `RECORD_MAX_GB=0` (the
default) nothing is deleted and recordings grow without bound, so prune them
yourself (a cron job removing files older than N days, say). Allow for the
current files on top of the budget: up to 256 MiB per stream until the next
rotation, where every stream counts: received, sent and fisb, each of
skycue-rawrec's two raw files, and each pingStation input's capture and raw
files. Rough growth:

- **Received stream:** about 450 bytes per observation for JSONL and CSV
  together. readsb emits roughly one observation per aircraft per second, so
  100 aircraft in view is about 45 kB/s, 160 MB/hour, 4 GB/day.
- **Sent stream:** smaller, bounded by `MIN_INTERVAL_S` per aircraft (at most
  one update per aircraft per `MIN_INTERVAL_S`, plus keyframes every
  `KEYFRAME_INTERVAL_S`).
- **pingStation capture** (`"capture":true`): one line per datagram or TCP
  block, about twice the bytes received plus some 80 bytes. The raw frames of
  a `pingstation_json` input (`RECORD_RAW=yes`) are about 30 to 90 bytes per
  frame.
- **Raw Beast capture** (`RECORD_RAW=yes`): about 15-25 bytes per Mode S
  message. A busy site decodes on the order of 1,500 messages a second
  (the bundled 120 s sample has about 177,000), which is about 30 kB/s,
  110 MB/hour, 2.5 GB/day. Raw UAT lines are far smaller.

If the disk fills, skycued's recorder and skycue-rawrec drop data and retry
with a backoff from 5 s to 300 s (see [recording-format.md](recording-format.md))
rather than stop, creating at most one extra file per stream however long it
lasts; cues keep being forwarded.

## Curated lists

`ALLOW_LIST_FILE` and `BLOCK_LIST_FILE` name plain-text files, read when
skycued starts and again within about 2 s of an edit:

```
# one entry per line; '#' starts a comment
a1b2c3        # an address: six hex digits, any case
N123AB        # a registration (a US N-number also matches its ICAO address)
G-ABCD
```

A blocked aircraft is never sent. An allow-listed one is sent whatever its
range or elevation. An aircraft that is on neither list is sent if it is
within `MAX_RANGE_NM` and above `MIN_ELEVATION_DEG`. An aircraft without a
position is never sent. A bad entry stops skycued with exit status 2, naming
the file and line.

## The web UI

skycued serves a live map on `WEB_PORT` (default 8080). Open
`http://<WEB_BIND>:<WEB_PORT>/` in a browser: with the defaults that is
`http://127.0.0.1:8080/` on the receiver itself. `WEB_BIND=127.0.0.1` (the
default) only accepts connections from that machine; set `WEB_BIND=0.0.0.0`
in `skycue.conf` (and restart skycued) to reach the page from another
machine, as `http://<receiver address>:8080/`. There is no login: anyone who
can reach the port can change the filters and lists, so open it only to a
network you trust. `WEB_PORT=0` turns the page off. The page loads nothing
from the internet; the API behind it is in [web-api.md](web-api.md).
`skycue-start.sh` prints the page's address after `start` or a replay, and
`status` shows it; with `WEB_BIND=0.0.0.0` (or `::`) it prints this
machine's host name instead, as `http://<host>:8080/ (WEB_BIND=0.0.0.0
listens on all interfaces)`. The page's files are in `<prefix>/share/skycue/www`
(Leaflet under `vendor/leaflet/`, ECharts under `vendor/echarts/`); the
clean-room build check
(`scripts/verify-offline-build.sh`, see [building-rhel.md](building-rhel.md))
fetches the page, Leaflet, ECharts and `/api/state` from a replaying skycued on
Rocky 8 and 9.

The left panel has two groups: **Output filters** (what is sent to the
cue clients: range, elevation and the allow and block lists, see
[Filters and lists](#filters-and-lists)) and **Display filters** (this
browser only). Display filters starts with a "Hide filtered" checkbox (off
by default, remembered per browser): when on, aircraft that are not `sent` or `allow_listed` are not
drawn and the panel shows how many are hidden. It only changes what this page
draws, not the filters, lists, recordings or cue outputs.

Under it, the **Trails** group sets how the aircraft trails are drawn (see
[Trails](#trails)).

Below that, the rest of Display filters narrows what this browser's map and
table show. The search matches the hex, callsign, registration or squawk
(`/` puts the cursor in it, Escape clears it); altitude, speed and range
take a minimum and a maximum (the "ground" box says whether aircraft on
the ground are included; their altitude counts as 0); the pills pick
decisions (sent, allow, range, elev, blocked) and classes (airline,
private, military, unknown); "emergency only" keeps
aircraft declaring an emergency or squawking 7500, 7600 or 7700; Category
picks the emitter category and Country the country of registration.
"Non-matching on the map" either hides the other aircraft or dims them to
faint symbols without labels or trails; the table leaves them out. The
section shows "showing N of M" and a Clear button, and its heading the
number of active settings. The settings are remembered per browser
(localStorage `skycue.view`) and combine with "Hide filtered"; the selected
aircraft always stays shown. The class and the country are estimates, from
the ICAO address, the callsign and the category (an address that is not an
ICAO address, such as a TIS-B track file, gets no country and no
address-based military class); the Track panel shows them as "Class (est.)"
and "Country". Like "Hide filtered", Display filters only changes what
this page shows, not the filters, lists, recordings or cue outputs.

**Saved filter sets.** The "Sets" row at the top of Display filters keeps
named sets of every Display filter setting together with "Hide filtered".
Choosing a set in the list applies it at once; "— current —" leaves the
settings as they are. **Save** asks for a name (up to 40 characters) and
saves the current settings under it; a name already taken is replaced
after a confirmation. **Delete** (after a confirmation) removes the chosen
set, and **⋯** holds Rename, Move up, Move down, Export… and Import….
Once the settings no longer equal the chosen set, the list shows
"<name> (modified)"; choosing the set again applies it again. The keys 1
to 9 apply the first nine sets and 0 clears the Display filters to their
defaults, like Clear (not while typing in a field). Up to 20 sets are kept,
in localStorage `skycue.viewSets`.

**Export…** downloads `skycue-settings-<date>.json` (the UTC date): the
filter sets and the alert settings (the rules, their parameters, the watch
list and Mute; see [Alerts](#alerts)), so one file carries a whole setup
to another browser. **Import…** reads such a file (at most 1 MB), says
what it holds and which of your sets a merge would replace, and offers
**Replace**, **Merge** and **Cancel**. Replace takes the file's sets and
alert settings instead of yours (a file without alert settings leaves
yours as they are). Merge keeps your sets in their order,
replaces the ones with a name the file also has, adds the file's other
sets at the end up to 20 (the rest are named as not imported), takes the
file's rules one by one (a rule the file lacks stays as it is) and adds
the file's watch list to yours; your Mute stays. Either way the strip
stays shown or hidden, and its drawer open or closed, as it was. The sets
and the alert settings are this browser's only: an import or an export is
never sent to skycued.

The header shows the counters: tracks known and forwarded, observations per
second, `inputs n/m` (enabled inputs connected / enabled inputs: green when
all are, else red; a click opens the [Receiver tab](#receiver-tab)), UAT
downlinks and uplinks per minute, cue clients, recorder drops and web
clients. To the right, **Weather**, **Receiver**, **Cues** and **Config**
open the side panel on that tab (the one whose tab is showing is
highlighted; a second click, or Escape, closes the panel). **Table**
swaps the map for the track table and **Stats** for the
[Stats view](#stats-view); while either is shown its button reads **Map**
and goes back. A red banner says when the page has lost skycued (the connection
closed, or no stats for 5 s); it reconnects by itself.

### Trails

The **Trails** group in Display filters (under "Hide filtered") sets how
the aircraft trails are drawn. Its settings are remembered per browser
with the rest of Display filters (localStorage `skycue.view`), are part of
the saved filter sets (and so of the number keys 1 to 9), and Clear (or the
0 key) puts them back to their defaults; they do not count in the heading's
number of active settings. The trail length is `TRAIL_SECONDS` in every setting.

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

A filter set saved before the Trails group existed takes the Trails
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
`TRAIL_SECONDS` of positions. A position the aircraft could not have flown
to (over 1500 kt from the trail's last point, taken over at least a second,
or more than 5 s earlier than it: a looped replay starting again, a bad
position, a reused hex) starts the trail afresh there instead of joining it;
one up to 5 s earlier (timestamp rounding, two inputs out of order) is left
out of the trail. The colour is skycued's decision for it:

| Colour | Decision | Meaning |
|--------|----------|---------|
| green | `sent` | within range and elevation: cues are sent |
| blue | `allow_listed` | on the allow list: sent whatever its range or elevation |
| grey | `out_of_range` | farther than `MAX_RANGE_NM`: not sent |
| orange | `below_elevation` | lower than `MIN_ELEVATION_DEG`: not sent |
| red | `blocked` | on the block list: never sent |
| purple | anything else | a decision this page does not know (a newer skycued) |

A marker glows briefly each time a cue is sent for it. The blue dashed
circle is `MAX_RANGE_NM` around the receiver (the white symbol); the orange
dotted circle is where an aircraft at the altitude chosen under "Elevation-
mask ring for aircraft at" sits exactly at `MIN_ELEVATION_DEG`: inside it,
an aircraft at that altitude is above the mask. Aircraft without a position
are only in the table.

**Measuring a distance.** The ruler button (title "Measure") under the
zoom buttons arms the ruler. Click the map (or an aircraft, a weather
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
  the `skycue.conf` value.
- **Allow list** and **Block list**: the entries in force, each with a ×
  to remove it; type a hex address or registration and press Add (or
  Enter) to add one.

These changes do not touch `skycue.conf` or the list files: they go to
`skycue.runtime.json` (the "overlay"), in `STATE_DIR` when that is set, else
next to `skycue.conf`, which skycued also reads at startup, so they survive
a restart. While the overlay holds
anything, "Unsaved changes: the running filters differ from skycue.conf and
the list files" is shown and **Save as baseline** is enabled. Save as
baseline (after a confirmation) writes the running values into
`MAX_RANGE_NM` / `MIN_ELEVATION_DEG` and into the list files, and clears
the overlay. With `STATE_DIR` empty (the bundle) the two values are
rewritten in `skycue.conf` itself; with `STATE_DIR` set (the packages)
`skycue.conf` is left untouched and they go to
`STATE_DIR/skycue.overrides.conf`, which skycued reads after `skycue.conf`
(see [Where the files are](#where-the-files-are)). What could not be saved
(for instance a list
whose `*_LIST_FILE` key is empty) is listed under the button and stays in the
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
[Receiver tab](#receiver-tab). An aircraft's switches to the map and
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
- **Input health** opens the [Receiver tab](#receiver-tab).
- **Reset statistics** asks first ("Reset all statistics? ... this cannot
  be undone."), then empties all of it (series, charts, coverage, the
  aircraft ledger and its evicted count) and saves the empty file at once.
  The header counters and `GET /api/stats` are the process's own counts and
  are not touched. Another reset within 5 s of the last one is refused
  ("Reset failed: the statistics were reset less than 5 s ago").

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
  need `RECEIVER_LAT`/`RECEIVER_LON`.
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

The ledger holds at most `STATS_AIRCRAFT_MAX` aircraft (default 20000).
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
  `skycue-coverage-<date>.csv`. It needs `RECEIVER_LAT`/`RECEIVER_LON`.

`<date>` is the UTC date of the data clock. The files are RFC 4180 CSV
(a header row, quotes where a cell needs them, CRLF line ends) in UTF-8
without a BOM, built in the page. Every chart card also has a small
"Save as PNG" icon at its top right, which saves the chart as
`skycue-<chart>-<date>.png` on the page's dark background.

For recordings, `skycue-stats`
([below](#analysing-recordings-skycue-stats)) still gives the same kind of
numbers offline, from the files alone.

### Statistics file

skycued keeps the statistics in `STATS_FILE`, by default
`skycue.stats.json` beside the overlay, `skycue.runtime.json`: in
`STATE_DIR` when that is set, else next to `skycue.conf`. A relative path
is taken from `STATE_DIR` when it is set, else from `skycue.conf`'s
directory. It reads the file at startup and logs `<file>: statistics of N
aircraft since <time>`. It writes the file every `STATS_SAVE_S` seconds
(default 300) when anything changed, at a clean stop, and at once on a
reset, so a crash loses at most the last interval. A failed save logs one
WARN and is retried every interval. `STATS_FILE=` (empty) keeps the
statistics in memory only: they start empty at every start. All three keys
are read at startup: a change needs a restart.

At the defaults a full file (20,000 aircraft, a full position grid, 24 h of
minutes and 30 days of hours) is about 6 MB, and reading it at startup
takes about 150 MB of memory for a moment. A file that cannot be read back
(malformed, a version this skycued does not know, or over 32 MiB) is
renamed `<file>.bad`, replacing an older one, with one ERROR line, and the
statistics start empty; the next save writes a new file. A `STATS_FILE`
that is not a regular file (a directory, `/dev/null`) is left alone: one
ERROR line, and the statistics are kept in memory only for that run. A save
that writes more than 24 MiB logs a WARN (at the 100,000-aircraft maximum
the file is roughly 27 MB, close to the 32 MiB a load accepts). A file with more
aircraft than `STATS_AIRCRAFT_MAX` (after lowering it) loses the oldest at
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

"Other rasters" in the layer control shows one more FIS-B raster at a time
(lightning, icing, turbulence, cloud tops) from a product dropdown; icing
and turbulence add an altitude band dropdown (default the lowest band with
data). The winds and raster picks stay until what they name is no longer
offered; a reconnect, which reloads everything, keeps them.

The "Radar opacity" slider at the bottom right of the map, beside the
radar's age, sets how opaque the NEXRAD layers ("Radar (regional)" and
"Radar (CONUS)") and the "Other rasters" product are drawn, from 0.2 to 1
(default 0.7); a radar older than 15 min (regional) or 30 min (CONUS) is
drawn at half of it. The slider is disabled, and dimmed, while none of
these is drawn (no mosaic on a layer that is on, no raster product). Its
value is remembered per browser (localStorage `skycue.radarOpacity`).

### Receiver tab

The side panel's third tab, after Track and Weather; **Receiver** or
`inputs n/m` in the header opens and closes it. It has three sections.

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
(`normal`, `comm errors`, `not transmitting`; a code the ICD does not list is
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

**Ground stations**: the FIS-B ground stations heard in the last 60 minutes,
with position, products received and when last heard; each is a triangle
on the map.

**Decoder health**: the FIS-B decoder's records held per kind, bad frames,
segmented products waiting, mosaic blocks, the last uplink's time, uplinks
dropped as duplicates of another input's, and the recorder's dropped
records (received / sent).

### Cues tab

The side panel's fourth tab (**Cues** in the header opens and closes
it): what skycued sends to the cue consumers. It has
four sections.

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

**Cue clients**: every open cue TCP connection, with its endpoint,
`accepted` (it connected to `CUE_TCP_PORT`) or `dialled` (one of
`CUE_TCP_CONNECT`), when it connected and the bytes written to it, then
every UDP target with the datagrams sent to it. The filter changes and the
clients are refreshed every 5 s while the tab is open.

### Config tab

The side panel's fifth tab (**Config** in the header opens and closes it).
Shows the configuration skycued is running with (`GET /api/config`, see
docs/web-api.md); nothing on it can be changed. Each `skycue.conf` key has
its running value and its **source**: `conf` (set in `skycue.conf`),
`default` (not set there, so the built-in default applies), `overrides`
(set in `skycue.overrides.conf` in `STATE_DIR`, which save as baseline
writes when `STATE_DIR` is set, and whose values replace `skycue.conf`'s),
`cli` (a command-line option such as `--web-port` overrides it) or
`overlay` (a range or elevation filter changed in the UI and not yet saved
as baseline). Keys marked restart take effect only when skycued restarts;
the four filter keys (`MAX_RANGE_NM`, `MIN_ELEVATION_DEG`,
`ALLOW_LIST_FILE`, `BLOCK_LIST_FILE`) apply at once. Above the keys are the
daemon's version, uptime, the config, inputs and overlay files (and
whether the overlay holds unsaved changes), the www and recording
directories with the files being recorded, the log level, how it runs
(**Runs**: a replay of a named file with its speed and looping, or live and
whether it connects to readsb and dump978, so a replay or `--no-readsb` /
`--no-uat` is plain even though the decoder keys still show their values),
and the report locations it holds: **Learned locations** (how many and the
`fisb-locations.json` they are kept in, or "not saved this run") and
**Operator locations** (how many were read from the resolved
`LOCATIONS_FILE`, with any rejected lines and ids over the cap).

The keys are grouped as Receiver site, Decoders, Cue outputs, Recording,
Filters, Web and FIS-B (a key skycued has that none of these names is
under Other). The tab asks skycued again each time it is opened and on
**Refresh**. **Copy as text** puts the whole configuration on the
clipboard in `skycue.conf` form: a `# <group>` line before each group, and
before each `KEY=value` line a comment naming its source (and `, restart`),
e.g. `# MAX_RANGE_NM: overlay` then `MAX_RANGE_NM=20`. Values with spaces
or other characters the file needs quoted are quoted (`"..."`, or `'...'`
when the value holds `"`, `$`, `` ` `` or `\`), so the text can go into a
`skycue.conf` as it is and both skycued and `skycue-start.sh` accept it. A
value the file cannot hold at all (one with both `'` and one of `"`, `$`,
`` ` ``, `\`) is left as a comment saying so, with the value, to set by
hand. Where the browser does not allow the clipboard (a page served over
plain HTTP from another machine, for one), the tab says "Copy failed" and
shows the text selected in a box to copy by hand.

### Map tiles (optional)

Without tiles the map is a dark background with a 1°/5° latitude/longitude
grid and a scale bar, which is all the page needs. For coastlines and
terrain underneath, make tiles on a connected machine with
`scripts/make-tiles.sh` and put them in `tiles/` under the map directory:
`MAP_DIR` when it is set in `skycue.conf` (`/var/lib/skycue/maps` in the
packages), else `<prefix>/share/skycue/www/`. The overlays
(`overlays/`, below) go in the same directory. With `MAP_DIR` set,
`/tiles/`, `/overlays/` and `/api/tiles-available` are read from it only,
never from the www directory, and a `MAP_DIR` that does not exist is
logged as a WARN at start and the map has no tiles or overlays. See
[scripts/make-tiles.md](../scripts/make-tiles.md) for the sizes and how
the offline bundle carries them. The page picks them up on its next load.

With the packages, or a source install of `skycue/`, `skycue-make-maps`
makes both the tiles and the overlays straight into `MAP_DIR` (read from
`/etc/skycue/skycue.conf`, or `--config FILE`, or given as `--map-dir DIR`):

```sh
sudo skycue-make-maps --around 150                   # the world coarse, 150 nm around the receiver fine
sudo skycue-make-maps --preset conus                 # add the contiguous US at middle zooms
sudo skycue-make-maps --around 51.5 -0.12 100 --dry-run   # another point; print the plan only
sudo skycue-make-maps --container --preset europe    # GDAL from a container image (podman or docker)
skycue-make-maps --list                              # the regions already there
```

`skycue-make-maps [--config FILE] [--map-dir DIR] [--cache DIR] [--container [IMAGE]] [--owner USER] [--dry-run]`
with one of `--preset conus|europe|world`, `--around RADIUS_NM` (the
receiver's `RECEIVER_LAT`/`RECEIVER_LON` from the config), `--around LAT LON
RADIUS_NM` or `--bbox W,S,E,N --zoom MINZ-MAXZ`; or `--list`. Runs add up:
each first bakes the whole world at zoom 0–5 (region `global`) unless
`tiles/meta.json` already has it, then the region asked for (`--around`:
zoom 6–9, at most 500 nm, the box clamped at the antimeridian and at ±85°
rather than wrapped, see make-tiles.md; `conus` and `europe`: zoom 6–8;
`world`: the base alone), then the overlays, with `scripts/make-tiles.sh` and
`scripts/make-overlays.sh` from `<prefix>/share/skycue/scripts/`. It needs a
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
0 done, 1 failure, 2 usage or config error, 3 a missing tool. With the
offline bundle, run `scripts/make-tiles.sh` and `scripts/make-overlays.sh`
from its source tree as above (`scripts/skycue-make-maps --map-dir DIR` works
there too, with the scripts beside it).

### Map style and overlays

The small box under the zoom and Measure buttons sets how the map looks; every choice is
remembered per browser and only changes what this page draws.

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
map directory (`MAP_DIR`, else `<prefix>/share/skycue/www/`) (the page asks for it once when it loads; without it the browser's
console shows that one request as a 404 and the two checkboxes are left out).
Make the overlays on a connected machine, or on any machine whose cache
already holds the three Natural Earth downloads:

```sh
scripts/make-overlays.sh                                   # needs ogr2ogr (GDAL) on PATH
scripts/make-overlays.sh --container ghcr.io/osgeo/gdal:ubuntu-small-3.9.3   # ogr2ogr in podman/docker
scripts/make-overlays.sh --dry-run                         # print the commands only
cp -r skycue/www/overlays build/stage/share/skycue/www/    # or <prefix>/share/skycue/www/, or MAP_DIR
```

`make-overlays.sh [--cache DIR] [--out DIR] [--container IMAGE] [--dry-run]`
downloads `ne_50m_admin_0_boundary_lines_land`,
`ne_50m_admin_1_states_provinces_lines` and `ne_10m_populated_places_simple`
(about 1.2 MB of zip files, public domain, each pinned by SHA-256) into the
cache (default `build/naturalearth/`) once, and writes `admin0.json`,
`admin1.json` and `cities.json` (compact GeoJSON, lines simplified to about
1 km; about 1 MB in all) and `meta.json` (each file's source, version,
SHA-256, feature count and size, and when it was made) to the output
directory (default `skycue/www/overlays/`, git-ignored).
`SKYCUE_NE_VECTOR_URL` points it at a mirror of the same zip files. The
offline bundle (`scripts/make-offline-bundle.sh`) carries the directory when
it exists. skycued serves it like the rest of the page, so a reload picks it
up without a restart.

## Watching the stream: skycue-dump

`skycue-dump` prints a skycue stream one line per frame (`HELLO`, `UPDATE`,
`DROP`, `HB`, or `BAD` for a frame that does not decode), plus event lines
(`CONN up|down`, `LOCAL-DROP` for a drop the client inferred itself):

```sh
bin/skycue-dump                              # connect to 127.0.0.1:30500 (CUE_TCP_PORT)
bin/skycue-dump --tcp radar-pc:30500 --json  # one JSON object per line
bin/skycue-dump --udp 0.0.0.0:40000          # receive what CUE_UDP_TARGETS sends here
bin/skycue-dump --tcp-listen 0.0.0.0:40001   # wait for skycued to dial in (CUE_TCP_CONNECT)
bin/skycue-dump --count 20 --timeout 30      # stop after 20 frames or 30 s
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
  CAT021 sac=0 sic=0 addr=A6452D lat=39.93328 lon=-88.38120 fl=390.25 squawk=2001 ident=ATN3597 gnd=-
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
bin/skycue-stats --config etc/skycue/skycue.conf /data/rec/received-20261101-120000.jsonl
bin/skycue-stats --lat 38.9 --lon -77.0 capture.jsonl.gz
bin/skycue-stats --config etc/skycue/skycue.conf --json received-*.jsonl sent-*.jsonl > stats.json
```

| option | default | meaning |
| --- | --- | --- |
| `--lat L --lon L` | | receiver position (overrides `--config`) |
| `--config FILE` | | take `RECEIVER_LAT`/`RECEIVER_LON` (and `RECEIVER_ALT_M` + `ANTENNA_HEIGHT_M` for elevation) from a skycue.conf |
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
   `MIN_ELEVATION_DEG` filter computes it, plus the count below 0°.
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
$ bin/skycue-stats --lat 38.9 --lon -77.0 readsb-sample.jsonl
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

With `FISB_ENABLE=yes` (the default) skycued decodes the FIS-B ground
uplinks that dump978-fa passes it (or that a replayed recording holds):
NEXRAD radar and the other weather rasters, METAR/TAF/PIREP/winds text,
and NOTAM/TFR, AIRMET, SIGMET and SUA reports with their shapes. It keeps
the current products until they expire, records them as `fisb-*.jsonl`
when `RECORD_FORMATS` includes `jsonl`, serves them under `/api/fisb/`, and
draws them on the web map (radar on by default; NOTAM / TFR, SIGMET,
AIRMET, CWA, SUA and the other rasters in the layer control) and in the
**Weather** tab of the side panel. `bin/skycue-fisb` decodes an uplink file
offline. What is decoded and what is not, the store's rules, the CLI and
the limits: [fisb.md](fisb.md). FIS-B is for situational awareness only,
never for flight decisions.

### Report locations: fisb-locations.json

METARs, TAFs, winds and PIREPs name a location id (`KPRG`) but carry no
coordinates. skycued learns positions from the NOTAM and TFR shapes it
decodes (airport-style ids only: `K???`, `C???`, `PA??`, `PH??`) and keeps
them in `fisb-locations.json` in `STATE_DIR` when that is set, else next
to `skycue.conf`: written with an atomic
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
`skycue.conf` is in the same directory (or that has the same `STATE_DIR`)
shares this one file, so a replay
run with the live configuration adds the positions it learns to the live
file (they are real positions, so this is harmless; give a replay its own
configuration directory, or its own `STATE_DIR`, to keep them apart).

`LOCATIONS_FILE` optionally names a CSV of your own (a relative path is
taken from `skycue.conf`'s directory; `id,lat,lon` per line;
`#` comments, blank lines and an `id,lat,lon` header are skipped), merged
under the learned positions: a learned position wins for the same id, and
the CSV rows are never written to `fisb-locations.json`. Lines that do not
parse are skipped with one WARN giving the count and the first line number;
a file that cannot be read stops skycued with exit status 2. At most
250,000 ids are read from it; the rest are ignored with one WARN giving
the count (a world-wide `make-locations.py` file is well under that, and
the script warns when its output is not).
`scripts/make-locations.py` builds one from OurAirports' public-domain
data ([scripts/make-locations.md](../scripts/make-locations.md)); the
result is a local file, never committed. Both are served as
`GET /api/fisb/locations` ([web-api.md](web-api.md#get-apifisblocations)).

## The consumer library

Programs that act on cues link `libskycue` (`skycue::skycue`), whose
`skycue::Client` receives the stream in any of skycued's three modes. Build
integration, the API, threading, drops and liveness: [libskycue.md](libskycue.md).

## For developers: sanitizers, fuzzing, static analysis, soak

None of this is needed to run skycue, and none of it is in the default build
or the offline bundle. Before changing a parser, the daemon's threading or
anything that runs for days, run the checks in
[hardening.md](hardening.md): `scripts/run-sanitizers.sh asan|tsan`,
`scripts/run-fuzz.sh`, `scripts/run-static.sh` and
`skycue/tests/soak/soak.sh`, with their options, skip rules and how to add a
fuzz harness for a new parser. What the first sweep found and fixed is in
[hardening-report-2026-11-03.md](hardening-report-2026-11-03.md).
