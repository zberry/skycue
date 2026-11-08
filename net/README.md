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
| `endpoint.hpp` | `Endpoint`, `EndpointFields`, `parseEndpoint()`, `describe()`: turns raw config strings into a validated endpoint; every error names the field at fault (`EndpointError`). Also `unmapV4()`, which turns an IPv4-mapped IPv6 address (`::ffff:a.b.c.d`) into the IPv4 address it carries (used by the peer filters). |
| `framer.hpp` | `LineFramer` (`\n`-delimited, `\r` dropped, empty lines skipped, an over-long line dropped whole and counted) and `DatagramFramer` (one datagram, one message). `out()` gets a pointer valid only during the call. |
| `buffer.hpp` | `Buf`, an immutable shared byte buffer: a message fanned out to N peers is stored once. A zero-length `Buf` is never sent: `Dialer::send`, `Listener::send`/`sendAll` and `UdpSender::send` drop it and count it in `dropped`. |
| `write_queue.hpp` | `WriteQueue`: the send side of one TCP connection (queued `Buf`s, gather writes, a byte cap), shared by `Dialer` and `Listener`. |
| `counters.hpp` | `Counters`: bytes and messages in and out, connects, rejected peers, drops, peer count, last error; also `DownReason` and `PeerDown` (why a connection ended). |
| `backoff.hpp` | `Backoff` (doubling delays, capped) and `RetryTimer` (one pending wait; a replaced or cancelled wait never fires). |
| `tcp_options.hpp` | `TcpOptions` and `applyTcpOptions()`: keepalive, nodelay, read chunk, send cap, backoff bounds, connect timeout. Best effort: a failing option is logged and skipped. |
| `dialer.hpp` | `Dialer`: one outbound TCP connection kept up; resolves, connects, reads in chunks, reconnects with backoff. Each resolved address gets `connect_timeout` to connect, then the next address is tried. `onFailure(why)` reports each failed attempt cycle (posted, never after `stop()`). `send()` is queued, and dropped and counted when not connected. |
| `listener.hpp` | `Listener`: a TCP server for many peers. Stream ids 1, 2, 3, ... never reused; per-peer send queue with a byte cap (over the cap the peer is closed); `close(stream)` closes one peer; optional single permitted source address; rebinds after a failure. |
| `udp_receiver.hpp` | `UdpReceiver`: datagrams from a unicast port or a multicast group; closes and retries after a bind, join or receive error. `onState(bool bound)` reports each bind (also a rebind) and each loss, posted; `receiveBufferBytes()` is the granted receive buffer. |
| `udp_sender.hpp` | `UdpSender`: one datagram per buffer to every target, unicast or multicast; non-blocking, a datagram the kernel cannot take is dropped and counted. A host name that resolves to several addresses is sent to at one only: the first IPv4 address, else the first (logged at debug). |
| `fanout.hpp` | `Fanout`: owns a dynamic set of listeners, dialers and UDP senders and delivers one `Buf` to all of them. A slow or absent destination drops on its own; `publish()` never blocks. Peer-up and peer-down hooks carry a Fanout-wide peer id and, for peer-up, the remote address and a send-to-that-peer function; `peerInfo(id)` gives that peer's send-side stats. Construct it with `Fanout()`. `closePeer(id)` closes one peer from the host's side. |
| `log.hpp` | The process-wide log sink (below). |

## How skycued uses it

skycued's hub (`skycued/src/hub/`) is the library's main client. Every input
and output is a `Dialer`, `Listener`, `UdpReceiver` or `UdpSender` made from
an `Endpoint`. For outputs the hub keeps one `Fanout` for the stream outputs
(TCP) and one for the datagram outputs (UDP) of each kind: an input hands a
message to the hub as a `Buf` once, the hub publishes it to the kind's
`Fanouts`, and each destination copies nothing and queues, sends or drops on
its own. A TCP consumer past the send cap is closed by its `Listener`; the
output's counters (`peers`, `bytes`, `messages`, `dropped`, `last_error`) are
its destination's counters. The cue outputs use the same pieces through the
Forwarder, which keeps the protocol duties (Hello, keyframes, heartbeats).

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
| TCP connect timeout (per address) | 10 s (our choice; `TcpOptions::connect_timeout`, zero or less disables it) |
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
- A dialer's "connected" notification (`onConnection(true)`) is made synchronously from the connect
  handler. Every other notification (a connection lost, a failed attempt, a peer down) is posted, never
  called from inside `send()`, `sendAll()` or `stop()`. After `stop()` returns, no callback runs
  (`Dialer::stopWithNotice()` is the exception: it still posts the loss of a connection that was up).
- `Listener`, `Dialer`, `UdpReceiver` and `UdpSender` are created with
  `create()` and held by `std::shared_ptr`. A `Listener` or `Dialer` must be
  `stop()`ped: dropping the last outside reference without `stop()` leaves it
  alive, because pending handlers hold it.
- `UdpSender::start()` resolves host names synchronously and may block; call
  it before serving.
- Every component must be `stop()`ped and released before the `io_context` it
  runs on is destroyed: sockets and timers must not outlive their service.
- Counters are monotonic across `stop()` and `start()`; only `peers` resets.
  `Fanout::counters()` sums `messages_out`, `bytes_out`, `dropped` and `peers`
  only.
- `Fanout` peer events: it takes over `Listener::onPeer` and `Dialer::onConnection` of every attached
  destination, so register peer events through `Fanout::onPeerUp` and `onPeerDown` only. Peer ids are unique
  across the Fanout. Destinations that are already connected when attached get no peer-up, and the peers of a
  detached or stopped destination get no peer-down, so a host that keeps per-peer state must clear it when it
  detaches or stops a destination. Peer-up runs synchronously in the accept or connect handler (before any later
  `publish` can reach that peer); peer-down is posted. `Fanout::onPeerDown(id, DownReason, why)` says why the
  connection went: `closed` (the peer ended the stream), `error` (a read or write failed; `why` has the
  text), `over_cap` (cut off for passing `send_cap`) or `shutdown` (the host closed it with
  `Fanout::closePeer(id)`, which shuts a listener's stream down or stops a dialer without redialling).
  `Listener::downInfo()` and `Dialer::downInfo()` give the same inside their own peer-down handlers.
- `Fanout` holds destinations by `shared_ptr` and does not start or stop them. Detach destinations before
  destroying the `Fanout`.
- `Listener::peerStats(stream)`, `Dialer::peerStats()` and `Fanout::peerInfo(id)` report bytes and messages
  written and bytes still queued for one connection (`PeerStats`); a connection that is gone has none. A host
  that wants to flush before stopping waits for `queued_bytes == 0` on the peers it still sees.

## Multicast on one port

An IPv4 receiver binds its group address, so receivers of different groups on
one port are isolated. An IPv6 receiver binds the wildcard: every receiver on
that port receives the datagrams of every group joined on it. Give each IPv6
group its own port.

## Log sink

The library logs through `skycue::net::log()`, which discards by default.
`setLogSink(fn)` installs a `void(LogLevel, const std::string& name, const
std::string& message)` function (`debug`, `info`, `warn`, `error`);
`name` is the `name` the component was created with (`TcpOptions::name` for
`Dialer` and, through `ListenOptions::tcp`, `Listener`; `UdpReceiveOptions::name`;
the third argument of `UdpSender::create`), empty when it has none, so a program
with several inputs can prefix each line with its own label. Lifecycle lines
(connected, listening, peer up and down, receiver bound, the loss of an
established connection, accept and UDP send failures, UDP targets skipped) are `debug`: the host words
those for its users. The first failed connect attempt since the last success is `warn`
(`<name>: connect <host>:<port>: <address>: <why>; retrying in <N> ms (further failures logged at debug level)`),
the retries after it `debug`; a failed bind of a listener or receiver follows the same rule.
`setLogSink(nullptr)` restores the
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
`net_backoff`, `net_dialer`, `net_listener`, `net_udp`, `net_write_queue` and `net_fanout`. They
are built when `SKYCUE_NET_BUILD_TESTS` is on (the default follows
`SKYCUE_BUILD_TESTS`). From a build directory:

```sh
ctest -R '^net_' --output-on-failure
```

The line framer also has a libFuzzer harness, `tests/fuzz/fuzz_net_line_framer.cpp`
in the repository (target `net_line_framer`, with `SKYCUE_FUZZ=ON`).
