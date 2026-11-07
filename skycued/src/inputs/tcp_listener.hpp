#pragma once

// TcpListener: accepts connections on bind:port (a pingStation pushing to
// the daemon, spec §5.2), up to kMaxPeers at once. Each connection is its own
// stream (ids 1, 2, ...); onStreamClosed(id) fires when it ends. One more
// connection than the cap, or one from an address other than `peer` (when
// set), is accepted and closed at once, counted in rejectedPeer() and noted
// in lastError. Connected means peers() > 0: onConnection(true) with the
// first peer, onConnection(false) when the last one leaves. A bind failure is
// reported and retried as in UdpListener. The acceptor sets reuse_address,
// which on Linux only allows binding past TIME_WAIT: a port another process
// listens on still shows as a bind failure. Accepted sockets use TCP
// keepalive, so a vanished peer frees its slot. An adapter over
// skycue::net::Listener.

#include "inputs/transport.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace skycued {

class TcpListener : public Transport {
public:
    static constexpr std::size_t kMaxPeers = 16;
    static constexpr std::size_t kChunkBytes = 16384;

    TcpListener(boost::asio::io_context& io, std::string bind, uint16_t port, const std::string& peer,
                std::chrono::milliseconds rebind_delay = std::chrono::seconds(5));
    ~TcpListener() override;

    void start() override;
    void stop() override;
    bool connected() const override;
    bool datagrams() const override { return false; }

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace skycued
