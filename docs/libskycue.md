# libskycue: the consumer library

## What it is

libskycue is a small C++17 static library that receives the cue stream sent
by skycued and keeps a table of live aircraft tracks for you. You create a
`skycue::Client`, tell it how to reach skycued (connect over TCP, wait for
skycued to connect over TCP, or receive UDP datagrams), register callbacks,
and it calls you with one `skycue::Track` per update, a `TrackId` plus a
`DropReason` when a track goes away, and `true`/`false` when the link comes
and goes. It handles framing, reconnecting, liveness and stale-track cleanup.
It depends on Boost.Asio (Boost 1.66 or newer) and the platform's threads
library only. Headers: `skycue/client.hpp` (the API), `skycue/types.hpp`
(`Track`, `TrackId` and the enums, every field documented there) and
`skycue/protocol.hpp` (the wire codec; most consumers never include it). The
byte layout is in [protocol.md](protocol.md).

## Getting it into your build

Link target in every mode: **`skycue::skycue`**. It carries the include path,
`cxx_std_17`, `Boost::system` and `Threads::Threads`. The library is static
but built as position-independent code, so it can go into a shared library
(a plugin) as well as an executable. CMake 3.16 or newer is enough for a
consumer.

**As a subdirectory** (copy or submodule `libskycue` into your tree;
it finds Boost and Threads itself if your project has not):

```cmake
cmake_minimum_required(VERSION 3.16)
project(cueing LANGUAGES CXX)

set(SKYCUE_INSTALL OFF)   # keep libskycue out of your own `cmake --install`
add_subdirectory(third_party/libskycue)

add_executable(cueing main.cpp)
target_link_libraries(cueing PRIVATE skycue::skycue)
```

`SKYCUE_INSTALL` (default `ON`) installs the headers, the archive and a CMake
package with your project; set it `OFF` (as above, or `-DSKYCUE_INSTALL=OFF`)
when you link libskycue into your own binaries and ship only those.

**As an installed package** (built and installed from the skycue project, or
from a subdirectory build with `SKYCUE_INSTALL=ON`, then
`-DCMAKE_PREFIX_PATH=<prefix>` when configuring yours):

```cmake
find_package(skycue REQUIRED)   # also finds Boost 1.66+ (system) and Threads
target_link_libraries(cueing PRIVATE skycue::skycue)
```

Both modes, and linking into a shared library, are exercised by
`tests/consumer-smoke/` on every test run.

## Using skycue from your own CMake tree

When your project vendors the whole `skycue/` directory (rather than only
`libskycue`), add it with `add_subdirectory()` and switch off
everything but the library. The worked example is
[`examples/parent-project/`](../examples/parent-project/README.md):
a separate project (`client_demo`, CMake 3.25 minimum, its own C++17 and
`-Wall -Wextra -Werror` settings) that is not part of the skycue build.

```cmake
set(SKYCUE_BUILD_DAEMON OFF)    # no skycued, skycue-* tools or web UI
set(SKYCUE_BUILD_TESTS OFF)     # no unit tests (default OFF when not top-level)
set(SKYCUE_BUILD_EXAMPLES OFF)  # no minimal_consumer (default OFF when not top-level)
set(SKYCUE_INSTALL OFF)         # keep libskycue out of your `cmake --install`
add_subdirectory(${SKYCUE_SOURCE_DIR} skycue)

add_executable(client_demo client_demo.cpp)
target_link_libraries(client_demo PRIVATE skycue::skycue)
```

The one target to link is **`skycue::skycue`**, as above. With these options
skycue compiles only libskycue and needs only Boost 1.66+ (`system`) and
threads: Boost.ProgramOptions, Boost.Test and zlib are not looked for.
Configuring the whole directory needs CMake 3.25 (skycue's own minimum).
Nothing outside `skycue/` is used, so a copy of that directory alone is
enough. The `parent_project` test builds the example against such a copy on
every test run.

## Choosing a transport

Pick the mode that matches how skycued is configured. `Config::host` and
`Config::port` default to `127.0.0.1` and `30500` (skycued's default
`cues.tcp_port`).

| `Config::transport` | skycued setting | `host` / `port` mean | Use when |
|---|---|---|---|
| `Transport::tcp` (default) | `cues.tcp_port` | skycued's address and listening port; `host` may be a name | the consumer can reach skycued; the usual choice |
| `Transport::tcp_listen` | `cues.tcp_connect = ["yourhost:port"]` | local bind address and port; `port = 0` picks one (read it with `localPort()`) | skycued must open the connection (firewall direction), or several skycued instances feed one consumer |
| `Transport::udp` | `cues.udp_targets = ["yourhost:port"]` | local bind address and port | one-way links, or where connection set-up is unwanted; tolerates loss, but see the FAQ |

TCP gives you the full table at once (a keyframe burst on every connection)
and detects loss of the link quickly. UDP has no handshake: you learn the
full table only at the sender's next periodic keyframe.

## A complete minimal example

This is `examples/minimal_consumer.cpp`, built with the skycue
project (`SKYCUE_BUILD_EXAMPLES`, default `ON` when skycue is the top-level
project) so it keeps compiling:

```cpp
// minimal_consumer: the smallest useful libskycue program. Prints one line per
// connection change, track update and track drop until SIGINT or SIGTERM.
//
//   minimal_consumer tcp HOST:PORT     connect to skycued (its cues.tcp_port)
//   minimal_consumer listen BIND:PORT  let skycued connect (its cues.tcp_connect)
//   minimal_consumer udp BIND:PORT     receive datagrams (its cues.udp_targets)
#include <skycue/client.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop = 0;
extern "C" void onSignal(int) { g_stop = 1; }

const char* reasonName(skycue::DropReason r)
{
    switch (r) {
    case skycue::DropReason::timeout: return "timeout";
    case skycue::DropReason::filtered: return "filtered";
    case skycue::DropReason::source_lost: return "source_lost";
    case skycue::DropReason::shutdown: return "shutdown";
    case skycue::DropReason::stale: return "stale";
    }
    return "other"; // a value added by a newer sender
}

} // namespace

int main(int argc, char** argv)
{
    using Transport = skycue::Config::Transport;
    const std::string mode = argc == 3 ? argv[1] : "";
    const std::string addr = argc == 3 ? argv[2] : "";
    const auto colon = addr.rfind(':');
    char* end = nullptr;
    const unsigned long port =
        colon == std::string::npos ? 0 : std::strtoul(addr.c_str() + colon + 1, &end, 10);

    skycue::Config cfg;
    bool ok = colon != std::string::npos && colon > 0 && end != addr.c_str() + colon + 1 &&
              *end == '\0' && port <= 65535;
    if (mode == "tcp")
        cfg.transport = Transport::tcp;
    else if (mode == "listen")
        cfg.transport = Transport::tcp_listen;
    else if (mode == "udp")
        cfg.transport = Transport::udp;
    else
        ok = false;
    if (!ok) {
        std::fprintf(stderr, "usage: %s tcp HOST:PORT | listen BIND:PORT | udp BIND:PORT\n", argv[0]);
        return 2;
    }
    cfg.host = addr.substr(0, colon);
    cfg.port = static_cast<uint16_t>(port);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // Callbacks run on the client's I/O thread (cfg.run_io_thread is true).
    skycue::Client client(cfg);
    client.onConnectionChange([](bool up) { std::printf("CONN %s\n", up ? "up" : "down"); });
    client.onTrackUpdate([](const skycue::Track& t) {
        const std::string alt = t.alt_baro_ft ? std::to_string(*t.alt_baro_ft) + "ft" : "-";
        std::printf("UPDATE %06x %-8s %.5f,%.5f %s%s\n", t.id.address, t.callsign.value_or("-").c_str(),
                    t.lat_deg, t.lon_deg, alt.c_str(), t.keyframe ? " keyframe" : "");
    });
    client.onTrackDropped([](skycue::TrackId id, skycue::DropReason r) {
        std::printf("DROP %06x %s\n", id.address, reasonName(r));
    });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    client.start();
    if (cfg.transport == Transport::tcp_listen) // 0: not bound yet, retrying
        std::printf("LISTEN port %u\n", static_cast<unsigned>(client.localPort()));
    while (!g_stop)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client.stop(); // no callback runs after this returns
    return 0;
}
```

Run with no arguments it prints the usage line and exits 2 (the ctest
`example_minimal_consumer_help` checks this).

## Lifecycle

1. **Construct** `skycue::Client client(cfg);`. Nothing happens on the network yet.
2. **Register callbacks**: `onTrackUpdate`, `onTrackDropped`,
   `onConnectionChange`, optionally `onFrame` and `onError`. Do this before
   `start()`; each is optional and replaces any earlier one.
3. **`start()`** opens the transport. It is effective once; further calls,
   and calls after `stop()`, do nothing. With `tcp_listen` the bind happens
   inside `start()`, so `localPort()` is valid when it returns (0 if the bind
   failed; it is then retried every `reconnect_interval`). `start()` never
   reports a network error: a skycued that is not there yet is simply
   retried. Register `onError` to hear about those retries (see
   [Diagnostics](#diagnostics)).
4. **Run.** Two modes, chosen by `Config::run_io_thread`:
   - **Thread mode** (`true`, the default): `start()` spawns one I/O thread.
     All network work, the track table and **every callback** run on it.
   - **Poll mode** (`false`): no thread is created. Nothing happens until you
     call `poll()`, which runs whatever handlers are ready and returns
     (`io_context::poll`); callbacks run inside `poll()` on your thread. Call
     it at least every `heartbeat_timeout` (see the FAQ).
5. **`stop()`** closes everything and, in thread mode, joins the I/O thread.
   It is idempotent, safe without `start()`, and may be called from inside a
   callback (it then does not join; the destructor does). No callback runs
   after `stop()` returns, and `stop()` itself fires none: in particular it
   does **not** deliver drops for the tracks still in the table. The
   destructor calls `stop()`.

`start()`, `stop()` and `poll()` belong to the owning thread. `trackCount()`
and `localPort()` are safe from any thread. The library has no getter for the
track table: keep your own copy, fed by the callbacks.

**Callbacks must not throw.** If one does, the client carries on exactly as
if it had returned. In thread mode the exception is discarded, and reported
to `onError` (with its `what()`) when you registered one. In poll mode the
first one is kept and rethrown from `poll()` after that call's
ready handlers have run; later ones are discarded until it has been
rethrown. **Never destroy the Client inside one of its own callbacks**; call
`stop()` there and destroy it afterwards from the owning thread.

Callbacks run one at a time on the I/O thread, so a slow callback delays
everything behind it, including liveness bookkeeping. Keep them short; hand
work to your own queue if it may block.

## What you will receive

On a **TCP** connection (either direction), in order:

1. `onConnectionChange(true)` as soon as the TCP connection is up.
2. A Hello (visible only through `onFrame`) giving the sender's keyframe and
   heartbeat intervals.
3. A **keyframe burst**: one `onTrackUpdate` with `keyframe == true` for
   every track skycued is currently forwarding.
4. Then **deltas**: an update for a track only when skycued's change
   detector fires. With skycued's defaults that is at once for a callsign,
   squawk, emergency, on-ground or ACAS advisory change; otherwise at most once per
   `cues.min_interval_s` (1 s) when the aircraft moved or its altitude, speed or
   track changed enough; and at least once per `cues.max_interval_s` (10 s).
5. Every `cues.keyframe_interval_s` (10 s) a **periodic keyframe**: every
   forwarded track again, `keyframe == true`.
6. Heartbeats (`onFrame` only) whenever nothing else was sent for the
   heartbeat interval (skycued: 1 s).

Over **UDP** there is no greeting: `onConnectionChange(true)` fires on the
first well-formed datagram, deltas arrive as they happen, and the full table
arrives with the next periodic keyframe, which skycued precedes with a Hello.

**Every `Track` carries the sender's full state** for that aircraft, keyframe
or not, so replace your stored copy instead of merging. An optional field
that turns `nullopt` means the value is no longer known. A keyframe update
means the same as any other; the flag tells you only why it was sent. Field
units and meanings are documented in `skycue/types.hpp`: degrees, feet, knots
and feet per minute; `position_time` is when the position was measured, on
the sender's clock (UTC).

Since protocol version 2 a `Track` also carries, each optional: `acas_ra`
(`AcasRaWire`: `ara`, and `flags` with bits 0-3 RAC, 4 RAT, 5 MTE, 6 active),
`heading_mag_deg`, `heading_true_deg`, `ias_kt`, `tas_kt`, `mach`, `roll_deg`,
`track_rate_dps`, `sel_alt_ft`, `sel_alt_fms_ft`, `qnh_mb`, `sel_heading_deg`,
`nav_modes` (bit 0 autopilot, 1 vnav, 2 althold, 3 approach, 4 lnav, 5 tcas),
`emergency` (a code) and `category` (e.g. `"A3"`). `emergencyName(code)` in
`types.hpp` turns the emergency code into `"none"`, `"general"`, `"medical"`,
`"minimum_fuel"`, `"no_comms"`, `"unlawful_interference"` or
`"downed_aircraft"` (`"unknown"` for any other code).

The first `onTrackUpdate` for a `TrackId` is how you learn of a new track;
there is no separate "new" callback.

## Drops and liveness

`onTrackDropped(id, reason)` means the track has left the library's table:
remove it from yours. If the aircraft comes back it arrives as a new update.

| `DropReason` | Sent by | Meaning | What to do |
|---|---|---|---|
| `timeout` | skycued | it stopped hearing the aircraft (`cues.track_timeout_s`, 60 s) | forget it |
| `filtered` | skycued | still heard, but range, elevation or block list now exclude it | forget it; it may return if it re-enters the filters |
| `source_lost` | skycued or the library | skycued lost its own radio decoder and the track aged out; **or** the library lost skycued (connection closed, liveness timeout, UDP silence) | forget it; data resumes when the source returns |
| `shutdown` | skycued | skycued is exiting cleanly | forget it; expect the connection to close next |
| `stale` | the library | no keyframe update for more than two keyframe intervals, i.e. it was missing from the periodic refreshes (a lost drop, or a sender bug) | forget it |

Treat an unknown reason value (a newer sender) like `source_lost`.

**Stale rule.** A track's stale clock is reset by every keyframe update and
by its first appearance; deltas do not reset it. It is dropped as `stale`
once that clock is older than 2 × the keyframe interval (the interval from
its connection's Hello, `default_keyframe_interval` before one). The check
runs every 1.5 × the interval, so with 10 s keyframes a track that misses
refreshes goes 20 to 35 s after its last one. Over UDP no track is dropped as
stale until the first Hello with a keyframe interval has arrived, again after
every loss of the source.

**Liveness.** Every frame of any type, heartbeats included, counts as a sign
of life.

- **TCP (both modes):** each connection has a watchdog of
  `max(heartbeat_timeout, 3 × heartbeat_interval of that connection's Hello)`,
  `heartbeat_timeout` until its Hello arrives. With skycued (1 s heartbeats)
  and the default 3 s `heartbeat_timeout` that is 3 s. On expiry the
  connection is closed as dead even though the socket is open; this catches
  a hung or vanished sender that TCP alone would not notice for a long time.
  `SO_KEEPALIVE` is set as a backstop.
- **UDP:** no well-formed datagram for `heartbeat_timeout` means the source
  is gone. (A Hello's heartbeat interval is not used over UDP.)

**When the link goes down** (TCP closed or timed out, UDP silent, or the last
`tcp_listen` connection gone) you get `onConnectionChange(false)` first, then
`onTrackDropped(id, source_lost)` for **every** track in the table, which is
then empty. With `tcp` the client redials after `reconnect_interval` (a fixed
2 s; there is no backoff), forever, and on success you get
`onConnectionChange(true)` and a fresh keyframe burst. UDP goes back to
connected on the next well-formed datagram. A stream with a bad magic or
version (not a skycue v1 or v2 sender) is closed the same way and redialled.

(The 0.5 s to 10 s doubling backoff in [protocol.md](protocol.md) is
skycued's, when it dials a `tcp_listen` consumer.)

## Multiple senders

**`tcp_listen`** accepts any number of skycued connections and merges them
into one table:

- Each connection has its own decoder, keyframe interval (from its Hello) and
  liveness watchdog.
- Each track is **owned** by the connection it last came from. An update from
  another connection takes it over.
- A `TrackDrop` is honoured **only from the owner**. Another connection
  dropping it means that skycued stopped forwarding it, not that the aircraft
  is gone, so it is ignored.
- The stale rule uses the owner's keyframe interval.
- A connection that closes, times out or sends a bad magic/version is closed
  on its own; the tracks it owns are dropped as `source_lost` and the others
  are untouched.
- "Connected" means at least one connection is open: `onConnectionChange(true)`
  on the first, `false` (then `source_lost` for every remaining track) when
  the last one closes. The listener keeps accepting; there is no dialling.

**`udp` is single-source.** Datagrams from any address feed the one table as
if they came from one sender, with one keyframe interval (the latest
Hello's). Bind one UDP port per sending skycued. **`tcp`** talks to one
skycued; use one `Client` per sender if you need several (and merge them
yourself), or `tcp_listen`.

## Cartesian state (ECEF)

`#include <skycue/ecef.hpp>` (namespace `skycue::geo`) turns a `Track`
(degrees, feet, knots) into a WGS-84 Earth-centred, Earth-fixed position (m)
and velocity (m/s), each with a 1-sigma error, and computes look angles
(azimuth, elevation, slant range) from a sensor site. skycued sends what it
received and does not extrapolate, so a state is exact at its
`position_time`; call `propagate(state, now)` if you want it moved forward.
Nothing in the library does that for you.

When a field is missing, a fixed ladder picks a substitute, and the state
names the rung it used (`altitude_source`, `velocity_source`,
`vertical_source`, `error_source`) so you can threshold on quality. The first
rung that applies wins:

| Ladder | Rungs, best first | Source enum |
|---|---|---|
| Height above the ellipsoid (sigma) | `alt_geom_ft` reported (15 m); `alt_geom_ft` with `alt_geom_uncertainty_ft` (that value); `alt_baro_ft` (`baro_sigma_m`, 300 m); `Options::default_height_m` (30 m); otherwise no state | `AltitudeSource`: `geom_reported`, `geom_estimated`, `baro`, `caller_default` |
| Horizontal velocity | `ground_speed_kt` and `track_deg`; derived from the anchor fix (needs `derive_velocity` and `EcefTracker`); none | `VelocitySource`: `reported`, `derived`, `none` |
| Vertical velocity (only with a horizontal one) | `vertical_rate_fpm`; derived from the anchor fix when both altitudes are the same kind (geometric with geometric, or barometric with barometric); zero | `VerticalSource`: `reported`, `derived`, `assumed_zero` |
| Horizontal error (sigma) | `nac_p`; `nic`; `default_horizontal_sigma_m` (1852 m) | `ErrorSource`: `nac_p`, `nic`, `default_` |

The anchor moves only when a derivation succeeds, so slow targets still get one after a few updates.

The library never invents a height: with no altitude rung, `toEcef()` and
`EcefTracker::update()` return `nullopt`. `toEcef()` is a pure function and
uses only the reported rungs. `EcefTracker` keeps the last state per track,
which is what the derived rungs need, and also drops tracks that go quiet
(`Options::stale_after`, applied when you call `expire()`). It is thread-safe,
so the Client's I/O thread can feed it while your own thread reads it.

There are two ways to take the result into a program, and both are shown as
complete examples in `examples/`:

| | Push | Pull |
|---|---|---|
| Client mode | poll mode (`run_io_thread = false`) | thread mode (the default) |
| How states reach you | `EcefTracker::onState` / `onDropped` callbacks, inside `poll()` on your thread | `Options::collect_changes`, then `takeChanges()` from your loop (apply `dropped` before `updated`) |
| Locking in your code | none, everything is on one thread | none, the tracker holds the lock |
| Pick it when | you want to react to each update with the lowest latency and no second thread | your loop already exists, runs on its own clock and wants batches |
| Example | `examples/ecef_consumer.cpp` | `examples/ecef_batch_consumer.cpp` |

`examples/ecef_consumer.cpp` keeps its callbacks thin (store, mark changed),
ticks at 10 Hz, picks the highest-elevation target every tick, and once a second calls
`expire()`, prints a summary line and, with `--sensor LAT,LON,H_M`, the pointing
line for the target propagated to now. A `~` in front of a value marks
it as derived rather than reported. Against a running skycued, with the sensor
at the receiver's position:

```
$ ecef_consumer --sensor 52.0,4.5,10 tcp 127.0.0.1:30500
POINT 4d012c az=345.7 el=25.3 range=27.2km age=3.1s alt=geom vel=reported err=nacp
SUMMARY tracks=208 vel(reported=203 derived=0 none=5) alt(geom=202 geom_est=5 baro=1 default=0)
```

Without `--sensor` it prints the states that changed instead of the pointing
line:

```
STATE 4a3121 pos=4114.676,508.265,4845.187km vel=63.7,235.4,-78.3m/s sigma=38,15m alt=geom vert=reported err=nacp
```

`examples/ecef_batch_consumer.cpp` registers only the two feed callbacks, and
once a second takes the batch with `takeChanges()`: `new` for a track seen for
the first time, `upd` for later states, `drop` with the reason. One entry
per track, however many updates arrived in between.

```
$ ecef_batch_consumer --sensor 52.0,4.5,10 tcp 127.0.0.1:30500
upd 392aef pos=4041.346,312.057,4921.305km vel=163.6,-61.7,-139.6m/s sigma=12,15m alt=geom err=nacp az=182.4 el=3.5 range=143.9km
```

## Tuning `Config`

| Field | Default | Effect |
|---|---|---|
| `transport` | `Transport::tcp` | see [Choosing a transport](#choosing-a-transport) |
| `host` | `"127.0.0.1"` | tcp: skycued's address (name or IP). udp, tcp_listen: local bind address (`0.0.0.0` for all IPv4 interfaces) |
| `port` | `30500` | tcp: skycued's port. udp, tcp_listen: local port; `0` picks a free one (`localPort()` reports it for tcp_listen) |
| `reconnect_interval` | 2000 ms | tcp: delay before every redial after a failure or disconnect. udp, tcp_listen: delay before retrying a failed bind, accept or receive |
| `default_keyframe_interval` | 10000 ms | keyframe interval assumed for the stale rule until a Hello gives one; clamped to at least 10 ms. Match it to the sender's `cues.keyframe_interval_s` if that differs |
| `heartbeat_timeout` | 3000 ms | udp: silence that means the source is gone. tcp, tcp_listen: lower bound of each connection's watchdog. Raising it tolerates longer stalls; on TCP, once a Hello has arrived, values below 3 × the sender's heartbeat interval have no effect |
| `run_io_thread` | `true` | `true`: thread mode. `false`: poll mode, everything happens inside `poll()` |

A Hello's keyframe interval (also clamped to at least 10 ms) replaces
`default_keyframe_interval`; an interval of 0 in a Hello is ignored.

## Diagnostics

- **`onFrame(const skycue::Frame&)`** fires for every frame that decodes,
  before the client acts on it: Hello and Heartbeat frames too, and an
  update's frame before its `onTrackUpdate`. It reports what arrived, not
  what was accepted: the payload may still fail its decoder and the type may
  be unknown. Decode it with the functions in `skycue/protocol.hpp`
  (`decodeHello`, `decodeHeartbeat`, ...). Same thread and exception rules
  as the other callbacks.
- **`onError(const std::string&)`** fires once for each failed attempt the
  client recovers from by itself, with the retry delay: a bind that failed
  (`tcp_listen: cannot listen on 0.0.0.0:30600: Address already in use;
  retrying in 2000 ms`, and the same for `udp: cannot bind ...`), a tcp
  resolve or connect that failed (`tcp: cannot connect to 127.0.0.1:30500:
  Connection refused; retrying in 2000 ms`), a failed accept or UDP receive;
  and, in thread mode, a callback exception that was swallowed (`a callback
  threw: <what()> (ignored; the client carries on)`). Log them; nothing else
  needs doing, since the client keeps retrying. A connection going down is
  not an error (that is `onConnectionChange(false)`). It runs on the same
  thread as the other callbacks; one that throws is not reported again. The
  wording may change between versions, so do not parse it.
- **`trackCount()`**: the number of tracks in the library's table, from any
  thread. Handy for a status line or a test.
- **`localPort()`**: the port a `tcp_listen` client is bound to, or 0.
- **`skycue-dump`** (in the skycue project) is the reference consumer: a
  pure libskycue program that prints every frame (`HELLO`, `UPDATE`,
  `DROP`, `HB`, `BAD`) and every event the library inferred (`CONN up|down`,
  `LOCAL-DROP` for a drop the client made itself). Run it with the same
  transport and address as your program to see what you should be getting;
  `--timeout S` exits 3 if nothing arrives. Its source,
  `tools/skycue-dump.cpp`, shows poll mode and `onFrame` in use.

## Compatibility promises

- The library sends **protocol version 2** and reads versions 1 and 2. A
  sender with another version byte is rejected (`bad_version`) and its
  connection closed; a version 1 library does the same to a version 2 sender.
- Within version 2 the sender may append fields to any frame, define the
  reserved TrackUpdate presence bits (26–31), and add frame types. This
  library ignores trailing bytes and unknown frame types, and at an unknown
  presence bit keeps the fields already read (all known ones come first).
- **Enumerations may grow.** `AddressType`, `Source` and `DropReason` values
  arrive exactly as sent, so a newer sender can deliver a value outside the
  ones in `types.hpp`. Give every `switch` a fallback and treat unknown
  values as `other` (for drops, as `source_lost`). An unknown `AddressType`
  is still a distinct, valid `TrackId`.
- Version 2 will not change the meaning, order or encoding of an existing
  field, frame type or enum value; an incompatible change gets a new version
  byte.
- The C++ API is at version 0.1.0 and may still change before 1.0; the
  installed CMake package accepts only the same major version.

## FAQ and pitfalls

**In poll mode my link keeps dropping with `source_lost`.** Frames that have
arrived but that `poll()` has not yet handled do not count for liveness, so
a gap between `poll()` calls longer than the watchdog (3 s by default) makes
a healthy link look dead: you get `onConnectionChange(false)`, a drop for
every track, a redial and a fresh burst. Poll every 100 ms or so, or raise
`heartbeat_timeout`. The same applies in thread mode to a callback that
blocks for seconds.

**My program crashes when it deletes the Client from a callback.** That is
not allowed. Call `stop()` in the callback (safe) and destroy the Client
from the owning thread afterwards.

**My exception from a callback vanished.** In thread mode exceptions from
callbacks are discarded. Catch inside the callback, or use poll mode, where
the first one is rethrown from `poll()`.

**UDP takes a long time to show all aircraft, or keeps ghosts.** A UDP
receiver gets the full table only at the next periodic keyframe, so with a
long `cues.keyframe_interval_s` a fresh receiver starts with only the aircraft
that happen to change. A lost `TrackDrop` datagram leaves a ghost until the
stale rule removes it, 2 to 3.5 keyframe intervals later; and until the
first Hello (sent just before each keyframe) no stale drops happen at all.
Keep the keyframe interval short (the default 10 s is reasonable) or use
TCP if this matters.

**`tcp` never connects and nothing tells me why.** The client redials every
`reconnect_interval` forever and reports only success (`onConnectionChange(true)`).
Check the address with `skycue-dump --tcp HOST:PORT --timeout 5`. Note that
`cues.tcp_port = 0` in skycued's config means skycued has **no listener**: use
`tcp_listen` with `cues.tcp_connect`, or UDP, instead. `Config::port = 0` is
meaningful only for `tcp_listen` and `udp` (pick a free port); for `tcp` it
connects nowhere.

**I listen with `port = 0`; which port do I tell skycued?** Read
`localPort()` after `start()`; 0 there means the bind failed and is being
retried.

**skycued disconnected my TCP consumer.** skycued closes a TCP client whose
unsent queue exceeds 1 MiB, i.e. one that stopped reading for a while
(typically a blocked callback). The client redials and receives a fresh
keyframe burst.

**Can I ask the client for the current table?** No; keep your own map keyed
by `TrackId` (`std::hash<TrackId>` and `operator<` are provided), insert or
replace on every update, erase on every drop.
