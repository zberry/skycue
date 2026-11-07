#pragma once

// Dialer: one outbound TCP connection that is kept up. It resolves, connects,
// reads in chunks and reconnects with doubling backoff when the peer goes
// away. All work and all callbacks run on the io_context's thread (it must be
// run by one thread); call every member from that thread, except connected(),
// which may be read from any. After stop() returns no callback is invoked.

#include "skycue/net/buffer.hpp"
#include "skycue/net/counters.hpp"
#include "skycue/net/tcp_options.hpp"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace skycue::net {

class Dialer : public std::enable_shared_from_this<Dialer> {
public:
    static std::shared_ptr<Dialer> create(boost::asio::io_context& io, std::string host, uint16_t port,
                                          TcpOptions o = {});
    ~Dialer();
    Dialer(const Dialer&) = delete;
    Dialer& operator=(const Dialer&) = delete;

    void onData(std::function<void(const uint8_t*, std::size_t)> h);
    void onConnection(std::function<void(bool up)> h); // true after connect, false when lost (not after stop)
    void start();
    void stop();
    bool connected() const;
    void send(Buf b); // queued; dropped and counted when not connected
    const Counters& counters() const;
    uint64_t attempts() const; // connection attempts started, successful or not

private:
    Dialer(boost::asio::io_context& io, std::string host, uint16_t port, TcpOptions o);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void connect();
    void lost(uint64_t gen, const std::string& why);
    void readLoop(uint64_t gen);
    void writeLoop(uint64_t gen);
};

} // namespace skycue::net
