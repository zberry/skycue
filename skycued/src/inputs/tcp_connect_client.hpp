#pragma once

// TcpConnectClient: connects to host:port (a pingStation in server mode,
// spec §5.2) and delivers what it reads in chunks of up to kChunkBytes.
// Reconnects with the LineClient backoff: 0.5 s doubling to 10 s, reset by a
// successful connect. Connected means it has a session. `stream` is the
// connection number (1, 2, ...); onStreamClosed(stream) fires on every
// disconnect, before onConnection(false). lastError holds the last resolve,
// connect or read failure.

#include "inputs/transport.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace skycued {

class TcpConnectClient : public Transport {
public:
    static constexpr std::size_t kChunkBytes = 16384;

    TcpConnectClient(boost::asio::io_context& io, std::string host, uint16_t port);
    ~TcpConnectClient() override;

    void start() override;
    void stop() override;
    bool connected() const override;
    bool datagrams() const override { return false; }

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace skycued
