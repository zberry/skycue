#pragma once

// UdpSender: sends each buffer as one datagram to every target, unicast or
// multicast. All members run on the io_context's thread (it must be run by
// one thread). One socket is opened per address family, at start(), only for
// the families the usable targets need. Sends are non-blocking: a datagram the
// kernel cannot take at once is dropped and counted. After stop() nothing is
// sent. A host name that resolves to several addresses is sent to at one
// only: the first IPv4 address, or the first address when there is none; the
// choice is logged at debug. An empty Buf is dropped and counted once per
// target.

#include "skycue/net/buffer.hpp"
#include "skycue/net/counters.hpp"

#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace skycue::net {

struct UdpTarget {
    std::string host;                       // address, hostname, or a multicast group
    uint16_t port = 0;
    std::string iface;  int ttl = 1;  bool loopback = true;   // multicast only
};

class UdpSender : public std::enable_shared_from_this<UdpSender> {
public:
    static std::shared_ptr<UdpSender> create(boost::asio::io_context& io, std::size_t max_datagram = 65507,
                                          std::string name = {}); // name: log prefix, see log.hpp
    ~UdpSender();
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    // Ignored once start() has been called (until stop()).
    void addTarget(const UdpTarget& t);
    // Resolves host names synchronously, so it may block on name resolution:
    // call it before serving, not from a latency-sensitive path.
    std::size_t start();                    // unresolvable targets are skipped (last_error says why, debug log); returns the usable count
    void stop();
    // One datagram per target; a no-op before start(). An oversize buffer
    // counts once in `dropped` per target. Failures are logged at debug only;
    // the host reads counters().last_error ("<target>: <reason>") and reports as it sees fit.
    void send(const Buf& b);
    std::size_t targets() const;            // usable targets after start()
    const Counters& counters() const;

    // For tests only: the native handle of the IPv4 socket (else the IPv6
    // one), -1 when none is open.
    int nativeHandleForTest() const;

private:
    UdpSender(boost::asio::io_context& io, std::size_t max_datagram, std::string name);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace skycue::net
