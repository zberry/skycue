#pragma once

// Dialer: one outbound TCP connection that is kept up. It resolves, connects,
// reads in chunks and reconnects with doubling backoff when the peer goes
// away. All work and all callbacks run on the io_context's thread (it must be
// run by one thread); call every member from that thread, except connected(),
// which may be read from any. Connection notifications are always delivered
// asynchronously (posted), never from inside send() or stop(). After stop() returns no callback is invoked.
//
// Each resolved address gets TcpOptions::connect_timeout to connect; on a
// timeout or a refusal the next address is tried, and when all fail the
// attempt is a failure and backs off.

#include "skycue/net/buffer.hpp"
#include "skycue/net/counters.hpp"
#include "skycue/net/tcp_options.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace skycue::net {

class Dialer : public std::enable_shared_from_this<Dialer> {
public:
    static std::shared_ptr<Dialer> create(boost::asio::io_context& io, std::string host, uint16_t port,
                                          TcpOptions o = {});
    // For tests: connects to the given endpoints in order, without resolving
    // anything, so a test can pin the order of the addresses.
    static std::shared_ptr<Dialer> create(boost::asio::io_context& io,
                                          std::vector<boost::asio::ip::tcp::endpoint> endpoints, TcpOptions o = {});
    ~Dialer();
    Dialer(const Dialer&) = delete;
    Dialer& operator=(const Dialer&) = delete;

    void onData(std::function<void(const uint8_t*, std::size_t)> h);
    void onConnection(std::function<void(bool up)> h); // true after connect, false when lost (not after stop)
    void start();
    void stop();
    bool connected() const;
    void send(const Buf& b); // queued; dropped and counted when not connected or empty (see buffer.hpp)
    const Counters& counters() const; // read on the io thread only
    uint64_t attempts() const;         // diagnostic: attempts started; io thread only

private:
    Dialer(boost::asio::io_context& io, std::string host, uint16_t port, TcpOptions o,
           std::vector<boost::asio::ip::tcp::endpoint> fixed);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void connect();
    void lost(uint64_t gen, const std::string& why);
    void readLoop(uint64_t gen);
    struct Attempt;
    void connectNext(uint64_t gen, const std::shared_ptr<boost::asio::ip::tcp::socket>& sock,
                     const std::shared_ptr<Attempt>& at);
    void connected(uint64_t gen, const std::shared_ptr<boost::asio::ip::tcp::socket>& sock);
};

} // namespace skycue::net
