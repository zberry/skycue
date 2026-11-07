#include "skycue/net/udp_sender.hpp"

#include "skycue/net/endpoint.hpp"
#include "skycue/net/log.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/multicast.hpp>
#include <boost/asio/ip/udp.hpp>

#include <net/if.h>

#include <chrono>
#include <optional>
#include <vector>

namespace skycue::net {

namespace asio = boost::asio;
using asio::ip::udp;

namespace {
constexpr std::chrono::seconds kErrorLogEvery{60};

unsigned interfaceIndex(const std::string& s)
{
    if (s.empty())
        return 0;
    if (s.find_first_not_of("0123456789") == std::string::npos)
        return static_cast<unsigned>(std::stoul(s));
    return ::if_nametoindex(s.c_str());
}
} // namespace

namespace {
struct Resolved {
    UdpTarget cfg;
    std::string name; // "host:port"
    udp::endpoint ep;
    bool multicast = false;
    std::chrono::steady_clock::time_point last_log{};
    bool logged = false;
};

// The multicast options last applied to a socket; they belong to the socket,
// not to a target, so they are set again whenever a send needs other ones.
struct McKey {
    std::string iface;
    int ttl = 0;
    bool loopback = true;
    bool operator==(const McKey& o) const { return iface == o.iface && ttl == o.ttl && loopback == o.loopback; }
};

struct FamilySocket {
    std::unique_ptr<udp::socket> sock;
    std::optional<McKey> applied;
};
} // namespace

struct UdpSender::Impl {
    Impl(asio::io_context& io_, std::size_t max) : io(io_), max_datagram(max) {}
    asio::io_context& io;
    std::size_t max_datagram;
    std::vector<UdpTarget> configured;
    std::vector<Resolved> resolved;
    FamilySocket v4, v6;
    bool running = false;
    Counters counters;

    FamilySocket& familyOf(const udp::endpoint& ep) { return ep.address().is_v6() ? v6 : v4; }

    // Sets the multicast options of `t` on its socket; empty on success.
    std::string applyMulticast(Resolved& t, FamilySocket& fs)
    {
        const McKey key{t.cfg.iface, t.cfg.ttl, t.cfg.loopback};
        if (fs.applied && *fs.applied == key)
            return {};
        boost::system::error_code ec;
        if (!t.cfg.iface.empty()) {
            if (t.ep.address().is_v4()) {
                const auto a = asio::ip::make_address(t.cfg.iface, ec);
                if (!ec && a.is_v4())
                    fs.sock->set_option(asio::ip::multicast::outbound_interface(a.to_v4()), ec);
                else if (!ec)
                    ec = asio::error::invalid_argument;
            } else {
                const unsigned idx = interfaceIndex(t.cfg.iface);
                if (idx == 0)
                    ec = asio::error::invalid_argument;
                else
                    fs.sock->set_option(asio::ip::multicast::outbound_interface(idx), ec);
            }
            if (ec)
                return "multicast interface " + t.cfg.iface + ": " + ec.message();
        }
        fs.sock->set_option(asio::ip::multicast::hops(t.cfg.ttl), ec);
        if (!ec)
            fs.sock->set_option(asio::ip::multicast::enable_loopback(t.cfg.loopback), ec);
        if (ec)
            return std::string("multicast options: ") + ec.message();
        fs.applied = key;
        return {};
    }

    // Logs at most once a minute per target; the first failure is a warning.
    void noteError(Resolved& t, const std::string& why)
    {
        counters.last_error = t.name + ": " + why;
        const auto now = std::chrono::steady_clock::now();
        if (!t.logged || now - t.last_log >= kErrorLogEvery) {
            log(LogLevel::warn, "udp send to " + counters.last_error);
            t.logged = true;
            t.last_log = now;
        }
    }
};

std::shared_ptr<UdpSender> UdpSender::create(asio::io_context& io, std::size_t max_datagram)
{
    return std::shared_ptr<UdpSender>(new UdpSender(io, max_datagram));
}

UdpSender::UdpSender(asio::io_context& io, std::size_t max_datagram) : impl_(std::make_unique<Impl>(io, max_datagram)) {}
UdpSender::~UdpSender() = default;

void UdpSender::addTarget(const UdpTarget& t) { impl_->configured.push_back(t); }
std::size_t UdpSender::targets() const { return impl_->resolved.size(); }
const Counters& UdpSender::counters() const { return impl_->counters; }
int UdpSender::nativeHandleForTest() const
{
    const auto& m = *impl_;
    if (m.v4.sock && m.v4.sock->is_open())
        return static_cast<int>(m.v4.sock->native_handle());
    if (m.v6.sock && m.v6.sock->is_open())
        return static_cast<int>(m.v6.sock->native_handle());
    return -1;
}

std::size_t UdpSender::start()
{
    auto& m = *impl_;
    if (m.running)
        return m.resolved.size();
    m.running = true;
    m.resolved.clear();
    udp::resolver resolver(m.io);
    for (const auto& cfg : m.configured) {
        Resolved r;
        r.cfg = cfg;
        r.name = cfg.host + ":" + std::to_string(cfg.port);
        boost::system::error_code ec;
        const auto addr = asio::ip::make_address(cfg.host, ec);
        if (!ec) {
            r.ep = udp::endpoint(addr, cfg.port);
        } else {
            const auto results = resolver.resolve(cfg.host, std::to_string(cfg.port), ec);
            if (ec || results.begin() == results.end()) {
                m.counters.last_error = "target " + r.name + ": " + (ec ? ec.message() : "no address");
                log(LogLevel::warn, "udp " + m.counters.last_error + " (skipped)");
                continue;
            }
            r.ep = results.begin()->endpoint();
        }
        r.multicast = isMulticast(r.ep.address());
        auto& fs = m.familyOf(r.ep);
        if (!fs.sock) {
            fs.sock = std::make_unique<udp::socket>(m.io);
            fs.sock->open(r.ep.protocol(), ec);
            if (!ec)
                fs.sock->non_blocking(true, ec);
            if (ec) {
                fs.sock.reset();
                m.counters.last_error = "target " + r.name + ": open socket: " + ec.message();
                log(LogLevel::warn, "udp " + m.counters.last_error + " (skipped)");
                continue;
            }
        }
        if (r.multicast) {
            const std::string why = m.applyMulticast(r, fs);
            if (!why.empty()) {
                m.counters.last_error = "target " + r.name + ": " + why;
                log(LogLevel::warn, "udp " + m.counters.last_error + " (skipped)");
                continue;
            }
        }
        log(LogLevel::info, "udp target " + r.name);
        m.resolved.push_back(std::move(r));
    }
    return m.resolved.size();
}

void UdpSender::stop()
{
    auto& m = *impl_;
    m.running = false;
    boost::system::error_code ec;
    for (auto* fs : {&m.v4, &m.v6}) {
        if (fs->sock)
            fs->sock->close(ec);
        fs->sock.reset();
        fs->applied.reset();
    }
    m.resolved.clear();
}

void UdpSender::send(Buf b)
{
    auto& m = *impl_;
    if (!m.running || !b)
        return;
    for (auto& t : m.resolved) {
        if (b->size() > m.max_datagram) {
            ++m.counters.dropped;
            m.noteError(t, "datagram of " + std::to_string(b->size()) + " bytes exceeds " +
                               std::to_string(m.max_datagram) + ", dropped");
            continue;
        }
        auto& fs = m.familyOf(t.ep);
        if (t.multicast) {
            const std::string why = m.applyMulticast(t, fs);
            if (!why.empty()) {
                ++m.counters.dropped;
                m.noteError(t, why);
                continue;
            }
        }
        boost::system::error_code ec;
        const std::size_t n = fs.sock->send_to(asio::buffer(*b), t.ep, 0, ec);
        if (ec) {
            ++m.counters.dropped;
            m.noteError(t, ec.message());
            continue;
        }
        ++m.counters.messages_out;
        m.counters.bytes_out += n;
    }
}

} // namespace skycue::net
