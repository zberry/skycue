#pragma once

// UdpReceiver: receives datagrams on one UDP port, unicast or from a
// multicast group. All work and all callbacks run on the io_context's thread
// (it must be run by one thread); call every member from that thread, except
// bound() and localPort(), which may be read from any. After stop() returns
// no callback is invoked. A bind or join failure, or a receive error, closes
// the socket and tries again after rebind_delay.
//
// Unicast never sets SO_REUSEADDR: a port held by someone else shows as a bind
// failure. Multicast sets it, binds the group address (IPv4) or the wildcard
// (IPv6) on the port and joins the group on `iface` (or the default), so
// several receivers on one host can share a group and port.
// Limitation: an IPv4 receiver is bound to its group address, so receivers of
// different groups on one port are isolated. An IPv6 receiver binds the
// wildcard, so every receiver on that port sees the datagrams of every group
// joined on it; use distinct ports for distinct IPv6 groups.

#include "skycue/net/counters.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace skycue::net {

struct UdpReceiveOptions {
    std::string peer;                       // "" = any
    std::string group;                      // "" = unicast; else join this multicast group
    std::string iface;                      // join on this local interface address; "" = default
    std::size_t receive_buffer = 4u << 20;  // requested SO_RCVBUF, best effort
    std::chrono::milliseconds rebind_delay{5000};
    std::string name;                       // log prefix, see log.hpp
};

class UdpReceiver : public std::enable_shared_from_this<UdpReceiver> {
public:
    // Throws std::invalid_argument when o.peer is not an IP address or o.group
    // is not a multicast address. `bind` is ignored for a multicast receiver.
    static std::shared_ptr<UdpReceiver> create(boost::asio::io_context& io, std::string bind, uint16_t port,
                                               UdpReceiveOptions o = {});
    ~UdpReceiver();
    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    // The pointer is valid only during the call.
    void onDatagram(std::function<void(const uint8_t*, std::size_t, const boost::asio::ip::udp::endpoint& from)> h);
    void start();
    void stop();
    bool bound() const;
    uint16_t localPort() const; // the bound port (port 0 asks the OS); 0 until bound
    int receiveBufferBytes() const; // the receive buffer the socket was granted; 0 until bound
    // Called (posted, so never from inside start(), stop() or a receive
    // handler) with true after each successful bind and join, also a rebind,
    // and with false when a bound socket is lost. A failed bind before the
    // first success is not an event. After stop() returns no callback runs.
    void onState(std::function<void(bool bound)> h);
    const Counters& counters() const; // read on the io thread only

    // For tests only: the native socket handle, -1 when not open.
    int nativeHandleForTest() const;

private:
    UdpReceiver(boost::asio::io_context& io, std::string bind, uint16_t port, UdpReceiveOptions o);
    struct Impl;
    struct Rx;
    std::unique_ptr<Impl> impl_;
    void bindAndJoin();
    void receiveLoop(uint64_t gen, const std::shared_ptr<Rx>& rx);
    void fail(const std::string& why);
    void notifyState(bool bound);
};

} // namespace skycue::net
