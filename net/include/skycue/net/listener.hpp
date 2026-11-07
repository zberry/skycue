#pragma once

// Listener: a TCP server that accepts many peers. Each peer gets a stream id
// (1, 2, 3, ... never reused within a Listener), a read loop and a send queue
// with a byte cap. All work and all callbacks run on the io_context's thread
// (it must be run by one thread); call every member from that thread, except
// listening() and localPort(), which may be read from any. Peer-down
// notifications are always delivered asynchronously (posted), never from
// inside send(), sendAll() or stop(). After stop() returns no callback is
// invoked.

#include "skycue/net/buffer.hpp"
#include "skycue/net/counters.hpp"
#include "skycue/net/tcp_options.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace skycue::net {

struct ListenOptions {
    TcpOptions tcp;
    std::size_t max_peers = 16;
    std::string peer;                                  // "" = any; else only this source address is accepted
    std::chrono::milliseconds rebind_delay{5000};
    bool fail_fast = false;                            // start() throws std::runtime_error on the first bind failure
};

class Listener : public std::enable_shared_from_this<Listener> {
public:
    // Throws std::invalid_argument when o.peer is not an IP address.
    static std::shared_ptr<Listener> create(boost::asio::io_context& io, std::string bind, uint16_t port,
                                            ListenOptions o = {});
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    void onData(std::function<void(uint64_t stream, const uint8_t*, std::size_t)> h);
    void onPeer(std::function<void(uint64_t stream, bool up, const boost::asio::ip::tcp::endpoint& who)> h);
    void start();
    void stop();
    bool listening() const;
    uint16_t localPort() const; // the bound port (port 0 asks the OS); 0 until bound
    void send(uint64_t stream, Buf b); // per-peer queue; over the cap the peer is closed (dropped++)
    void sendAll(const Buf& b);
    std::size_t peers() const;
    const Counters& counters() const; // read on the io thread only

private:
    Listener(boost::asio::io_context& io, std::string bind, uint16_t port, ListenOptions o);
    struct Impl;
    struct Session;
    std::unique_ptr<Impl> impl_;
    void bindAndListen(bool throwOnFailure);
    void acceptLoop(uint64_t gen, const std::shared_ptr<boost::asio::ip::tcp::acceptor>& acc);
    void adopt(const std::shared_ptr<Session>& s);
    void closeSession(const std::shared_ptr<Session>& s, const std::string& why);
    void readLoop(uint64_t gen, const std::shared_ptr<Session>& s);
    void writeLoop(uint64_t gen, const std::shared_ptr<Session>& s);
    void sendTo(const std::shared_ptr<Session>& s, Buf b);
};

} // namespace skycue::net
