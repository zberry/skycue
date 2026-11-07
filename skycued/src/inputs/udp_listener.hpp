#pragma once

// UdpListener: a UDP socket bound to bind:port (spec §5.1, §5.2). Each onData
// is one datagram (stream 0). Connected means bound. A bind failure sets
// lastError ("bind 0.0.0.0:4000: Address already in use") and is retried
// after `rebind_delay`. With `peer` set, datagrams from any other address are
// counted in rejectedPeer() and dropped. The socket does not set
// reuse_address, so a port another process holds shows as a bind failure.
// It asks for a 4 MiB receive buffer (best effort) so a burst survives a slow consumer.
// An adapter over skycue::net::UdpReceiver.

#include "inputs/transport.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace skycued {

class UdpListener : public Transport {
public:
    // Larger than any UDP payload, so the input can tell a datagram over 64 KiB.
    static constexpr std::size_t kBufferBytes = 65537;

    UdpListener(boost::asio::io_context& io, std::string bind, uint16_t port, const std::string& peer,
                std::chrono::milliseconds rebind_delay = std::chrono::seconds(5));
    ~UdpListener() override;

    void start() override;
    void stop() override;
    bool connected() const override;
    bool datagrams() const override { return true; }
    // The receive buffer the socket was granted (0 until bound); io_context thread.
    int receiveBufferBytes() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace skycued
