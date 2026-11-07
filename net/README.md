# skycue::net

A small C++17 network library on Boost.Asio. It is the one place where
skycue's inputs and outputs open sockets: a program describes each input or
output as an endpoint (a transport, an address, a port and a few options),
and the library accepts or dials TCP connections, receives and sends UDP
datagrams (unicast or multicast), cuts streams into messages, reconnects with
backoff, and fans one message out to many destinations. It knows nothing
about what the bytes mean; the embedding program parses them. Inputs and
outputs are hubs of equal parts: any number of sources in, any number of
sinks out, all handled on one io thread.

## Components

| Header | Contract |
|---|---|
| `endpoint.hpp` | `Endpoint`, `EndpointFields`, `parseEndpoint()`, `describe()`: turns raw config strings into a validated endpoint; every error names the field at fault (`EndpointError`). |
| `framer.hpp` | `LineFramer` (`\n`-delimited, `\r` dropped, empty lines skipped, an over-long line dropped whole and counted) and `DatagramFramer` (one datagram, one message). `out()` gets a pointer valid only during the call. |
| `buffer.hpp` | `Buf`, an immutable shared byte buffer: a message fanned out to N peers is stored once. |
| `counters.hpp` | `Counters`: bytes and messages in and out, connects, rejected peers, drops, peer count, last error. |
| `backoff.hpp` | `Backoff` (doubling delays, capped) and `RetryTimer` (one pending wait; a replaced or cancelled wait never fires). |
| `tcp_options.hpp` | `TcpOptions` and `applyTcpOptions()`: keepalive, nodelay, read chunk, send cap, backoff bounds. Best effort: a failing option is logged and skipped. |
| `dialer.hpp` | `Dialer`: one outbound TCP connection kept up; resolves, connects, reads in chunks, reconnects with backoff. `send()` is queued, and dropped and counted when not connected. |
| `listener.hpp` | `Listener`: a TCP server for many peers. Stream ids 1, 2, 3, ... never reused; per-peer send queue with a byte cap (over the cap the peer is closed); optional single permitted source address; rebinds after a failure. |
| `udp_receiver.hpp` | `UdpReceiver`: datagrams from a unicast port or a multicast group; closes and retries after a bind, join or receive error. |
| `udp_sender.hpp` | `UdpSender`: one datagram per buffer to every target, unicast or multicast; non-blocking, a datagram the kernel cannot take is dropped and counted. |
| `fanout.hpp` | `Fanout`: owns a dynamic set of listeners, dialers and UDP senders and delivers one `Buf` to all of them. A slow or absent destination drops on its own; `publish()` never blocks. A greeting hook runs when a TCP peer connects. |
| `log.hpp` | The process-wide log sink (below). |

## Endpoint fields

`parseEndpoint(fields, role)` takes raw strings (empty means absent) and
rejects any field that does not apply to the transport. Port 0 is accepted
only with `allow_port_zero` (tests that bind an ephemeral port).

| Field | Meaning | `tcp_listen` | `tcp_connect` | `udp` receive | `udp` send |
|---|---|---|---|---|---|
| `transport` | `tcp_listen`, `tcp_connect` or `udp` | required | required | required | required |
| `port` | 1 to 65535 | required | required | required | required |
| `bind` | local address, IP literal, default `0.0.0.0` | optional | no | optional (ignored for multicast) | no |
| `host` | remote address or host name | no | required | no | one of `host`, `group` |
| `peer` | only this source address is accepted; empty means any | optional | no | optional | no |
| `group` | multicast group address | no | no | optional (join) | one of `host`, `group` |
| `iface` | local interface address (IP literal) for multicast | no | no | optional, with `group` | optional, with `group` |
| `ttl` | multicast hop limit, 1 to 255, default 1 | no | no | no | optional, with `group` |
| `loopback` | deliver own multicast to this host (`true`, `false`, `yes`, `no`, `1`, `0`), default `true` | no | no | no | optional, with `group` |

`describe()` renders an endpoint for logs, for example `tcp_listen 0.0.0.0:30005`
or `udp 239.1.1.5:4001 via 192.0.2.10 ttl 4`; IPv6 addresses are in brackets.

## Pinned values

| Setting | Value |
|---|---|
| TCP keepalive | on; Linux idle 30 s, interval 10 s, 3 probes |
| TCP nodelay | on |
| Read chunk | 16384 bytes |
| Per-connection send cap | 1 MiB queued; above it the connection is closed and `dropped` is counted |
| Reconnect backoff | 500 ms doubling to 10 s |
| Listener `max_peers` | 16 |
| Listener and UDP receiver rebind delay | 5 s |
| `LineFramer` max line | 65536 bytes |
| UDP receive buffer | 4 MiB requested (best effort) |
| UDP datagram size | at most 65507 bytes; larger buffers are dropped and counted per target |
| Multicast `ttl` | 1; `loopback` on |

## Threading and lifetime

- Every component works on one `boost::asio::io_context`, which must be run
  by a single thread. All callbacks run on that thread, and every member is
  to be called from it, except the few documented as readable anywhere
  (`connected()`, `listening()`, `bound()`, `localPort()`).
- `counters()` and `attempts()` are read on the io thread only.
- Notifications (connection up or down, peer down) are posted, never called
  from inside `send()`, `sendAll()` or `stop()`. After `stop()` returns, no
  callback runs.
- `Listener`, `Dialer`, `UdpReceiver` and `UdpSender` are created with
  `create()` and held by `std::shared_ptr`. A `Listener` or `Dialer` must be
  `stop()`ped: dropping the last outside reference without `stop()` leaves it
  alive, because pending handlers hold it.
- `UdpSender::start()` resolves host names synchronously and may block; call
  it before serving.
- `Fanout` holds destinations by `shared_ptr` and does not start or stop them.
  Register peer events through `Fanout::onPeerUp` and `onPeerDown`, not
  through `Listener::onPeer`, and detach listeners before destroying the
  `Fanout`.

## Log sink

The library logs through `skycue::net::log()`, which discards by default.
`setLogSink(fn)` installs a `void(LogLevel, const std::string&)` function
(`debug`, `info`, `warn`, `error`); `setLogSink(nullptr)` restores the
default. The sink may be called from any thread, so it must be thread-safe.

## Using it on its own

The directory needs nothing from the rest of the repository. Boost (1.66 or
later, `system`) and Threads are found by its `CMakeLists.txt` when the
enclosing project has not already done so:

```cmake
add_subdirectory(path/to/net net)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE skycue::net)
```

```cpp
#include "skycue/net/endpoint.hpp"
```

`tests/scripts/test_net_standalone.sh` builds a copy of `net/` alone this
way.

## Tests

The tests are Boost.Test programs in `tests/`: `net_endpoint`, `net_framer`,
`net_backoff`, `net_dialer`, `net_listener`, `net_udp` and `net_fanout`. They
are built when `SKYCUE_NET_BUILD_TESTS` is on (the default follows
`SKYCUE_BUILD_TESTS`). From a build directory:

```sh
ctest -R '^net_' --output-on-failure
```

The line framer also has a libFuzzer harness, `tests/fuzz/fuzz_net_line_framer.cpp`
in the repository (target `net_line_framer`, with `SKYCUE_FUZZ=ON`).
