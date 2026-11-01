#pragma once

// skycue client: receives the skycue wire protocol over TCP or UDP and keeps
// a table of live tracks.

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
    enum class Transport { tcp, udp } transport = Transport::tcp;
    std::string host = "127.0.0.1"; // tcp: server to connect to; udp: local bind address
    uint16_t port = 30500;
    std::chrono::milliseconds reconnect_interval{2000};
    std::chrono::milliseconds default_keyframe_interval{10000}; // until a Hello says otherwise
    std::chrono::milliseconds heartbeat_timeout{3000};          // udp: no frame for this long -> source_lost
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace skycue
