#pragma once

// skycue client: receives the skycue wire protocol over TCP (connecting to
// skycued, or listening for skycued to connect) or UDP and keeps a table of
// live tracks.

#include <skycue/protocol.hpp>
#include <skycue/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace skycue {

struct Config {
    // tcp: connect to skycued; tcp_listen: accept connections from any number
    // of skycued instances (CUE_TCP_CONNECT); udp: bind and receive datagrams.
    enum class Transport { tcp, udp, tcp_listen } transport = Transport::tcp;
    std::string host = "127.0.0.1"; // tcp: server to connect to; udp, tcp_listen: local bind address
    uint16_t port = 30500;          // tcp_listen, udp: 0 picks an ephemeral port (tcp_listen: see localPort)
    // tcp: fixed delay before every redial (no backoff). tcp_listen, udp:
    // delay before retrying a bind (or an accept, or a receive) that failed.
    std::chrono::milliseconds reconnect_interval{2000};
    // Used for the stale check until a Hello says otherwise; at least 10 ms.
    std::chrono::milliseconds default_keyframe_interval{10000};
    // Liveness (see "Liveness" below): udp: no frame for this long ->
    // source_lost. tcp, tcp_listen: the minimum per-connection timeout.
    std::chrono::milliseconds heartbeat_timeout{3000};
    // true: start() spawns an I/O thread. false: nothing happens except
    // inside poll(), which the owner must call often (see Liveness).
    bool run_io_thread = true;
};

// Threading: all network I/O, the track table and every callback run on one
// thread, the I/O thread that start() spawns (run_io_thread == true) or the
// thread calling poll() (run_io_thread == false). Register callbacks before
// start(). start(), stop() and poll() are called from the owning thread;
// stop() may also be called from inside a callback. No callback is invoked
// after stop() returns, and stop() does not invoke any callback itself.
// Destroying a Client from inside one of its own callbacks is not allowed.
//
// Exceptions: a callback that throws does not disturb the client, which
// carries on as if the callback had returned. In poll mode the first such
// exception is kept and rethrown from poll() once that call's ready handlers
// have run (later ones are discarded until it has been rethrown); in thread
// mode exceptions from callbacks are discarded, each reported to onError.
//
// Track table: an update stores the track and fires onTrackUpdate; a
// TrackDrop for a known track erases it and fires onTrackDropped. A track
// whose last keyframe update (or first appearance) is older than two keyframe
// intervals is dropped as stale. When the connection goes down,
// onConnectionChange(false) fires, then source_lost for every track.
//
// tcp and udp are single-source: one stream, one keyframe interval (the
// latest Hello's). udp: datagrams from any sender feed the one table as if
// they came from one source, so one skycued per udp port; and since a UDP
// receiver may start mid-stream, no track is dropped as stale until the first
// Hello (sent before every keyframe burst) has said what the interval is.
//
// tcp_listen: every accepted connection (peer) has its own stream decoder,
// keyframe interval (from its own Hello, default_keyframe_interval before
// one) and liveness watchdog; frames from all of them feed the one track
// table. Each track is owned by the peer it last came from: an update from a
// different peer takes it over, the stale check uses the owner's keyframe
// interval, and a TrackDrop is honoured only from the owner (another peer
// dropping it means that skycued stopped forwarding it, not that it is gone).
// A peer that sends a frame with a bad magic or version, or falls silent
// (see Liveness), is closed on its own. When a peer closes, the tracks it
// owns are dropped as source_lost. The client is "connected" while at least
// one peer is open: true on the first, false (then source_lost for every
// remaining track) when the last one closes. There is no reconnecting; the
// listener keeps accepting. If the address cannot be bound, binding is
// retried every reconnect_interval.
//
// Liveness: heartbeats are the liveness signal in every transport; every
// received frame of any type counts. udp: no datagram for heartbeat_timeout
// -> onConnectionChange(false), then source_lost for every track. tcp and
// tcp_listen: each connection has a watchdog with timeout
// max(heartbeat_timeout, 3 x the heartbeat_interval_ms of that connection's
// Hello), heartbeat_timeout until a Hello arrives. On expiry a tcp
// connection is treated exactly like one the peer closed (connection false,
// source_lost, reconnect after reconnect_interval); a tcp_listen connection
// is closed on its own. SO_KEEPALIVE is set on every TCP socket as a backstop.
// In poll mode, poll at least every heartbeat_timeout: frames that arrived
// but were not yet handled do not count, so a long gap between polls can
// make a live connection look dead.
//
// Raw frames: onFrame (optional, default none) fires for every frame that
// decodes (TCP: each frame of the stream; UDP: each well-formed datagram),
// before the client acts on it, so it sees Hello and Heartbeat frames too,
// and an update's frame before onTrackUpdate. It reports what arrived, not
// what the client accepted: payloads may still fail their decoder, and
// `type` may be unknown. Same thread and exception rules as the others.
//
// Errors: onError (optional, default none) gets one human-readable line for
// each problem the client handles by itself, on the same thread as the other
// callbacks: a failed bind (tcp_listen, udp), resolve or connect (tcp), accept
// or receive, each with the retry delay ("...; retrying in 2000 ms"), once
// per failed attempt; and, in thread mode, a callback exception that was
// swallowed ("a callback threw: <what()>..."). Nothing needs to be done about
// any of them: the client keeps retrying and carrying on. A connection that
// goes down is not an error (onConnectionChange reports it). An onError that
// throws is not reported again (thread mode: discarded; poll mode: kept for
// poll() like any other callback's). The texts are for logs; do not parse them.
class Client {
public:
    explicit Client(Config config);
    ~Client(); // stops
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void onTrackUpdate(std::function<void(const Track&)> cb);
    void onTrackDropped(std::function<void(TrackId, DropReason)> cb);
    void onConnectionChange(std::function<void(bool connected)> cb);
    void onFrame(std::function<void(const Frame&)> cb);
    void onError(std::function<void(const std::string& message)> cb);

    // start() is effective once; later calls, and calls after stop(), do nothing.
    void start();
    // Idempotent; safe without start().
    void stop();
    // run_io_thread == false: runs ready handlers (io_context::poll). Does
    // nothing in thread mode or once stopped.
    void poll();
    size_t trackCount() const; // for tests and diagnostics; safe from any thread
    // tcp_listen: the port listened on (useful with port 0) once bound; start()
    // binds before it returns, unless the bind fails. 0 otherwise. Safe from
    // any thread.
    uint16_t localPort() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace skycue
