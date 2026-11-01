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
    std::chrono::milliseconds reconnect_interval{2000};
    std::chrono::milliseconds default_keyframe_interval{10000}; // until a Hello says otherwise
    // Liveness (see "Liveness" below): udp: no frame for this long ->
    // source_lost. tcp, tcp_listen: the minimum per-connection timeout.
    std::chrono::milliseconds heartbeat_timeout{3000};
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
// mode exceptions from callbacks are discarded.
//
// Track table: an update stores the track and fires onTrackUpdate; a
// TrackDrop for a known track erases it and fires onTrackDropped. A track
// whose last keyframe update (or first appearance) is older than two keyframe
// intervals is dropped as stale. When the connection goes down,
// onConnectionChange(false) fires, then source_lost for every track.
//
// tcp_listen: every accepted connection has its own stream decoder; frames
// from all of them feed the one track table (an update from any connection is
// authoritative, a Hello from any sets the keyframe interval). A connection
// that sends a frame with a bad magic or version is closed on its own. The
// client is "connected" while at least one connection is open: true on the
// first, false (then source_lost for every track) when the last one closes.
// There is no reconnecting; the listener keeps accepting. If the address
// cannot be bound, binding is retried every reconnect_interval.
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
