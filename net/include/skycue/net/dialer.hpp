#pragma once

// Dialer: one outbound TCP connection that is kept up. It resolves, connects,
// reads in chunks and reconnects with doubling backoff when the peer goes
// away. All work and all callbacks run on the io_context's thread (it must be
// run by one thread); call every member from that thread, except connected(),
// which may be read from any. The "connected" notification (onConnection(true))
// is made synchronously from the connect handler, so a host can queue a greeting
// before anything else is sent. The "lost" notification (onConnection(false))
// and onFailure are always posted, never made from inside send() or stop().
// After stop() returns no callback is invoked.
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
    // Test support: connects to the given endpoints in order, without
    // resolving anything, so a test can pin the order of the addresses.
    // Not for production use.
    static std::shared_ptr<Dialer> create(boost::asio::io_context& io,
                                          std::vector<boost::asio::ip::tcp::endpoint> endpoints, TcpOptions o = {});
    ~Dialer();
    Dialer(const Dialer&) = delete;
    Dialer& operator=(const Dialer&) = delete;

    void onData(std::function<void(const uint8_t*, std::size_t)> h);
    void onConnection(std::function<void(bool up)> h); // true (synchronously) after connect, false (posted) when lost (not after stop)
    // Why the connection in the lost notification now being delivered went
    // away; valid only inside the onConnection handler call for `up == false`.
    const PeerDown& downInfo() const;
    // Called once per failed attempt cycle: the lookup failed, or every resolved
    // address failed or timed out. `why` is the text of counters().last_error
    // ("resolve h: ...", "connect h:p: <addr>: <reason>"). Posted like the
    // connection events, and never called after stop(). The Dialer retries
    // by itself; call stop() from the handler for a one-shot probe.
    void onFailure(std::function<void(const std::string& why)> h);
    void start();
    void stop();
    // Like stop(), but a connection that is up still gets its notification
    // (posted, DownReason::shutdown), for a host that closes it deliberately.
    void stopWithNotice();
    bool connected() const;
    void send(const Buf& b); // queued; dropped and counted when not connected or empty (see buffer.hpp)
    const Counters& counters() const; // read on the io thread only
    // Send-side stats of the current connection (zeros when not connected).
    PeerStats peerStats() const;
    // The remote address of the current or latest connection; a default endpoint before the first.
    boost::asio::ip::tcp::endpoint peerEndpoint() const;
    uint64_t attempts() const;         // diagnostic: attempts started; io thread only

private:
    Dialer(boost::asio::io_context& io, std::string host, uint16_t port, TcpOptions o,
           std::vector<boost::asio::ip::tcp::endpoint> fixed);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void connect();
    void lost(uint64_t gen, const std::string& why, DownReason reason = DownReason::error);
    void readLoop(uint64_t gen);
    struct Attempt;
    void connectNext(uint64_t gen, const std::shared_ptr<boost::asio::ip::tcp::socket>& sock,
                     const std::shared_ptr<Attempt>& at);
    void connected(uint64_t gen, const std::shared_ptr<boost::asio::ip::tcp::socket>& sock);
};

} // namespace skycue::net
