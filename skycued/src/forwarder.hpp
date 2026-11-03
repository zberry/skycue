#pragma once

// Forwarder: sends skycue frames to TCP clients (those that connect to its
// listening port, and those it connects to itself) and to a fixed list of UDP
// targets, with periodic keyframe bursts and heartbeats.

#include <skycue/types.hpp>

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace skycued {

struct ForwarderConfig {
    uint16_t tcp_port = 30500; // TCP port to listen on; 0: no listener
    bool tcp_listen_ephemeral = false; // listen on a port the OS picks, ignoring tcp_port (tests; see tcpPort)
    std::vector<std::pair<std::string, uint16_t>> tcp_connect_targets; // host, port: dialled, kept connected
    std::vector<std::pair<std::string, uint16_t>> udp_targets; // numeric host (or resolvable name), port
    std::chrono::milliseconds keyframe_interval{10000};
    std::chrono::milliseconds heartbeat_interval{1000};
    std::string sender_name = "skycued";
    size_t max_client_queue_bytes = 1 << 20; // a TCP client with more unsent bytes is disconnected
};

// "addr:port" for a numeric address, an IPv6 one in brackets ("[::1]:30500").
std::string endpointText(const std::string& address, uint16_t port);

// One open TCP connection, accepted or dialled (see Forwarder::tcpClients).
struct CueTcpClient {
    std::string endpoint;                                   // the peer "addr:port"
    bool dialled = false;                                   // one of tcp_connect_targets, else accepted
    std::chrono::system_clock::time_point connected_since;  // wall clock at accept/connect
    uint64_t bytes_sent = 0;                                // bytes whose async_write completed
};

// One resolved UDP target (see Forwarder::udpTargets).
struct CueUdpTarget {
    std::string endpoint;         // "host:port" as configured
    uint64_t datagrams_sent = 0;  // sends completed without error
};

// Threading: the Forwarder lives on the caller's io_context. Every method,
// the constructor and the destructor are called from the thread running that
// io_context (or while it is not running); there are no locks.
//
// TCP: on accept a client gets Hello, then one keyframe of every track the
// provider returns, then the normal stream. Each client has its own outbound
// queue; a client whose queue exceeds max_client_queue_bytes is closed.
// Each of tcp_connect_targets is dialled at start(); once connected it is
// treated exactly like an accepted client (Hello, keyframe, queue limit,
// counted by tcpClientCount). A failed dial, or the connection closing for
// any reason, dials again after a delay that starts at 0.5 s and doubles to
// at most 10 s, back to 0.5 s after every successful connect.
// UDP: one datagram per frame per target. Each target also gets a Hello at
// start() and right before every keyframe burst, so a UDP receiver learns the
// keyframe interval (it has no connection on which to be greeted).
// Every keyframe_interval all provider tracks are sent with keyframe=true to
// every destination. When no frame has been broadcast for heartbeat_interval,
// a Heartbeat is broadcast. "Broadcast" means sent to all TCP clients and UDP
// targets; the Hello and keyframe sent to a single new client do not count.
class Forwarder {
public:
    using KeyframeProvider = std::function<std::vector<skycue::Track>()>;

    Forwarder(boost::asio::io_context& io, ForwarderConfig config);
    ~Forwarder(); // closes everything at once, without shutdown drops
    Forwarder(const Forwarder&) = delete;
    Forwarder& operator=(const Forwarder&) = delete;

    // Listens (unless tcp_port is 0), starts dialling the connect targets and
    // starts the timers. Throws std::runtime_error if the TCP port cannot be
    // bound. Effective once; does nothing after stop().
    void start();
    // Broadcasts TrackDrop(shutdown) for every track the provider returns,
    // stops accepting, dialling and the timers, and closes every socket once
    // its queued data is written (at most ~2 s later). Idempotent.
    void stop();

    // To all TCP clients and UDP targets. The caller sets keyframe=false.
    // Ignored before start() and after stop().
    void sendUpdate(const skycue::Track& track);
    void sendDrop(skycue::TrackId id, skycue::DropReason reason);

    // Supplies every currently forwarded track; the Forwarder marks each one
    // keyframe=true. Set before start().
    void setKeyframeProvider(KeyframeProvider provider);

    size_t tcpClientCount() const; // accepted and outbound connections
    size_t udpTargetCount() const; // UDP targets that resolved at start(); 0 before
    std::vector<CueTcpClient> tcpClients() const; // open sessions, by session id
    std::vector<CueUdpTarget> udpTargets() const; // resolved targets, config order
    // Frames handed to a destination: one per frame per TCP client or UDP target.
    uint64_t framesSent() const;
    // Port the TCP server listens on (useful with tcp_listen_ephemeral); 0
    // before start() or with no listener.
    uint16_t tcpPort() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_; // shared with pending handlers, so they never outlive it
};

} // namespace skycued
