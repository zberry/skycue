#include "skycue/net/endpoint.hpp"

#include <boost/system/error_code.hpp>

#include <cstddef>

namespace skycue::net {

namespace {

namespace ip = boost::asio::ip;

[[noreturn]] void fail(const std::string& msg)
{
    throw EndpointError(msg);
}

bool parseIp(const std::string& s, ip::address& out)
{
    boost::system::error_code ec;
    out = ip::make_address(s, ec);
    return !ec;
}

// Whole decimal number in [lo, hi]; false on anything else.
bool parseInt(const std::string& s, long lo, long hi, long& out)
{
    if (s.empty() || s.size() > 9)
        return false;
    long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (c - '0');
    }
    if (v < lo || v > hi)
        return false;
    out = v;
    return true;
}

bool validHostname(const std::string& h)
{
    if (h.empty() || h.size() > 253)
        return false;
    for (char c : h) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

void reject(const std::string& value, const char* field, const char* why)
{
    if (!value.empty())
        fail(std::string(field) + " " + why);
}

void requireIpLiteral(const std::string& value, const char* field)
{
    ip::address a;
    if (!parseIp(value, a))
        fail(std::string(field) + " must be an IP address: '" + value + "'");
}

void requireMulticast(const std::string& value, const char* field)
{
    ip::address a;
    if (!parseIp(value, a))
        fail(std::string(field) + " must be an IP address: '" + value + "'");
    if (!isMulticast(a))
        fail(std::string(field) + " must be a multicast address: '" + value + "'");
}

std::string hostPort(const std::string& host, uint16_t port)
{
    boost::system::error_code ec;
    const ip::address a = ip::make_address(host, ec);
    const bool v6 = !ec && a.is_v6();
    return (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

} // namespace

bool isMulticast(const ip::address& a)
{
    return a.is_multicast();
}

const char* transportName(Transport t)
{
    switch (t) {
    case Transport::tcp_listen: return "tcp_listen";
    case Transport::tcp_connect: return "tcp_connect";
    case Transport::udp: return "udp";
    }
    return "udp";
}

std::optional<Transport> parseTransport(std::string_view s)
{
    if (s == "tcp_listen")
        return Transport::tcp_listen;
    if (s == "tcp_connect")
        return Transport::tcp_connect;
    if (s == "udp")
        return Transport::udp;
    return std::nullopt;
}

Endpoint parseEndpoint(const EndpointFields& f, Role role, bool allow_port_zero)
{
    Endpoint e;

    if (f.transport.empty())
        fail("transport is required (tcp_listen, tcp_connect or udp)");
    const auto t = parseTransport(f.transport);
    if (!t)
        fail("transport must be tcp_listen, tcp_connect or udp: '" + f.transport + "'");
    e.transport = *t;

    if (f.port.empty())
        fail("port is required");
    long port = 0;
    if (!parseInt(f.port, allow_port_zero ? 0 : 1, 65535, port))
        fail(std::string("port must be an integer from ") + (allow_port_zero ? "0" : "1") + " to 65535: '" +
             f.port + "'");
    e.port = static_cast<uint16_t>(port);

    switch (e.transport) {
    case Transport::tcp_listen:
        if (!f.bind.empty()) {
            requireIpLiteral(f.bind, "bind");
            e.bind = f.bind;
        }
        if (!f.peer.empty()) {
            requireIpLiteral(f.peer, "peer");
            e.peer = f.peer;
        }
        reject(f.host, "host", "does not apply to tcp_listen");
        reject(f.group, "group", "does not apply to tcp_listen");
        reject(f.iface, "iface", "does not apply to tcp_listen");
        reject(f.ttl, "ttl", "does not apply to tcp_listen");
        reject(f.loopback, "loopback", "does not apply to tcp_listen");
        break;

    case Transport::tcp_connect: {
        if (f.host.empty())
            fail("host is required for tcp_connect");
        ip::address a;
        if (!parseIp(f.host, a) && !validHostname(f.host))
            fail("host must be an IP address or a host name: '" + f.host + "'");
        e.host = f.host;
        reject(f.bind, "bind", "does not apply to tcp_connect");
        reject(f.peer, "peer", "does not apply to tcp_connect");
        reject(f.group, "group", "does not apply to tcp_connect");
        reject(f.iface, "iface", "does not apply to tcp_connect");
        reject(f.ttl, "ttl", "does not apply to tcp_connect");
        reject(f.loopback, "loopback", "does not apply to tcp_connect");
        break;
    }

    case Transport::udp:
        if (role == Role::receive) {
            if (!f.bind.empty()) {
                requireIpLiteral(f.bind, "bind");
                e.bind = f.bind;
            }
            if (!f.peer.empty()) {
                requireIpLiteral(f.peer, "peer");
                e.peer = f.peer;
            }
            if (!f.group.empty()) {
                requireMulticast(f.group, "group");
                e.group = f.group;
            }
            if (!f.iface.empty()) {
                if (f.group.empty())
                    fail("iface applies to a multicast group only");
                requireIpLiteral(f.iface, "iface");
                e.iface = f.iface;
            }
            reject(f.host, "host", "does not apply to a udp receiver");
            reject(f.ttl, "ttl", "does not apply to a udp receiver");
            reject(f.loopback, "loopback", "does not apply to a udp receiver");
        } else {
            if (f.host.empty() == f.group.empty())
                fail(f.host.empty() ? "exactly one of host or group is required for a udp sender"
                                    : "host and group are exclusive for a udp sender");
            if (!f.host.empty()) {
                ip::address a;
                if (!parseIp(f.host, a) && !validHostname(f.host))
                    fail("host must be an IP address or a host name: '" + f.host + "'");
                e.host = f.host;
            } else {
                requireMulticast(f.group, "group");
                e.group = f.group;
            }
            if (!f.ttl.empty()) {
                if (f.group.empty())
                    fail("ttl applies to a multicast group only");
                long ttl = 0;
                if (!parseInt(f.ttl, 1, 255, ttl))
                    fail("ttl must be an integer from 1 to 255: '" + f.ttl + "'");
                e.ttl = static_cast<int>(ttl);
            }
            if (!f.loopback.empty()) {
                if (f.group.empty())
                    fail("loopback applies to a multicast group only");
                const std::string& l = f.loopback;
                if (l == "true" || l == "yes" || l == "1")
                    e.loopback = true;
                else if (l == "false" || l == "no" || l == "0")
                    e.loopback = false;
                else
                    fail("loopback must be true, false, yes, no, 1 or 0: '" + l + "'");
            }
            if (!f.iface.empty()) {
                if (f.group.empty())
                    fail("iface applies to a multicast group only");
                requireIpLiteral(f.iface, "iface");
                e.iface = f.iface;
            }
            reject(f.bind, "bind", "does not apply to a udp sender");
            reject(f.peer, "peer", "does not apply to a udp sender");
        }
        break;
    }
    return e;
}

std::string describe(const Endpoint& e)
{
    std::string target;
    switch (e.transport) {
    case Transport::tcp_listen:
        target = hostPort(e.bind, e.port);
        break;
    case Transport::tcp_connect:
        target = hostPort(e.host, e.port);
        break;
    case Transport::udp:
        if (!e.group.empty())
            target = hostPort(e.group, e.port);
        else if (!e.host.empty())
            target = hostPort(e.host, e.port);
        else
            target = hostPort(e.bind, e.port);
        break;
    }
    std::string s = std::string(transportName(e.transport)) + " " + target;
    if (!e.iface.empty())
        s += " via " + e.iface;
    if (!e.group.empty() && e.ttl != 1)
        s += " ttl " + std::to_string(e.ttl);
    return s;
}

} // namespace skycue::net
