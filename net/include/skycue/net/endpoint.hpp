#pragma once

// Endpoint: where a network input or output listens, connects or sends, and
// parseEndpoint(), which turns the raw strings of a config or a file into one
// and rejects anything that does not fit the transport. Every error message
// names the field at fault ("ttl applies to a multicast group only").

#include <boost/asio/ip/address.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace skycue::net {

enum class Transport { tcp_listen, tcp_connect, udp };
enum class Role { receive, send };

struct Endpoint {
    Transport transport = Transport::tcp_connect;
    std::string bind = "0.0.0.0";   // tcp_listen, udp receive
    std::string host;               // tcp_connect, udp send (unicast)
    uint16_t port = 0;
    std::string peer;               // "" = any
    std::string group;              // "" = none; a multicast address otherwise
    std::string iface;              // "" = OS default
    int ttl = 1;
    bool loopback = true;
};

// Raw strings from a config or a file; "" = absent.
struct EndpointFields {
    std::string transport, bind, host, port, peer, group, iface, ttl, loopback;
};

struct EndpointError : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

// Throws EndpointError. Port 0 is accepted only with allow_port_zero (tests
// that bind an ephemeral port).
Endpoint parseEndpoint(const EndpointFields& f, Role role, bool allow_port_zero = false);

// "tcp_listen 0.0.0.0:30005", "udp 239.1.1.5:4001 via 192.0.2.10 ttl 4";
// IPv6 addresses in brackets.
std::string describe(const Endpoint& e);

// An IPv6 interface given as an index ("2") or a name ("eth0"); 0 for "" or an unknown name.
unsigned interfaceIndex(const std::string& s);

bool isMulticast(const boost::asio::ip::address& a);

// An IPv4-mapped IPv6 address (::ffff:a.b.c.d) as the IPv4 address it carries;
// any other address unchanged. Peer filters compare addresses after this.
boost::asio::ip::address unmapV4(const boost::asio::ip::address& a);

// "tcp_listen" | "tcp_connect" | "udp"
const char* transportName(Transport t);
std::optional<Transport> parseTransport(std::string_view s);

} // namespace skycue::net
