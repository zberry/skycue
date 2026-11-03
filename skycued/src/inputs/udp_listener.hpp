#pragma once

// UdpListener: a UDP socket bound to bind:port (spec §5.1, §5.2). Each onData
// is one datagram (stream 0). Connected means bound. A bind failure sets
// lastError ("bind 0.0.0.0:4000: Address already in use") and is retried
// after `rebind_delay`. With `peer` set, datagrams from any other address are
// counted in rejectedPeer() and dropped. The socket does not set
// reuse_address, so a port another process holds shows as a bind failure.

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

    UdpListener(boost::asio::io_context& io, std::string bind, uint16_t port, std::string peer,
                std::chrono::milliseconds rebind_delay = std::chrono::seconds(5));
    ~UdpListener() override;

    void start() override;
    void stop() override;
    bool connected() const override;
    bool datagrams() const override { return true; }

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace skycued
