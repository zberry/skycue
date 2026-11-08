#include "skycue/net/udp_receiver.hpp"

#include "skycue/net/backoff.hpp"
#include "skycue/net/endpoint.hpp"
#include "skycue/net/log.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/multicast.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/socket_base.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <vector>

namespace skycue::net {

namespace asio = boost::asio;
using asio::ip::udp;

namespace {
constexpr std::size_t kDatagramBuffer = 65537; // one byte more than any UDP payload, so nothing is cut silently

} // namespace

// Everything one open socket needs while an operation on it is in flight.
struct UdpReceiver::Rx {
    explicit Rx(asio::io_context& io) : sock(io), buf(kDatagramBuffer) {}
    udp::socket sock;
    std::vector<uint8_t> buf;
    udp::endpoint from;
};

struct UdpReceiver::Impl {
    Impl(asio::io_context& io_, std::string b, uint16_t p, UdpReceiveOptions o_)
        : io(io_), bind(std::move(b)), port(p), opts(std::move(o_)), rebind(io_)
    {
    }
    asio::io_context& io;
    std::string bind;
    uint16_t port;
    UdpReceiveOptions opts;
    bool has_peer_filter = false;
    asio::ip::address peer_addr;
    asio::ip::address group_addr; // set for a multicast receiver
    RetryTimer rebind;
    std::shared_ptr<Rx> rx;
    uint64_t gen = 0; // changes on start() and stop(); voids handlers of an earlier run
    bool running = false;
    bool failed_before = false;
    std::optional<std::chrono::steady_clock::time_point> peer_noted;
    std::atomic<bool> up{false};
    std::atomic<uint16_t> bound_port{0};
    std::atomic<int> rcvbuf{0};  // the receive buffer the socket was granted; 0 until bound
    uint64_t stop_epoch = 0;     // changes on stop(); suppresses state notifications posted earlier
    std::function<void(bool)> on_state;
    std::function<void(const uint8_t*, std::size_t, const udp::endpoint&)> on_datagram;
    Counters counters;
};

std::shared_ptr<UdpReceiver> UdpReceiver::create(asio::io_context& io, std::string bind, uint16_t port,
                                                 UdpReceiveOptions o)
{
    return std::shared_ptr<UdpReceiver>(new UdpReceiver(io, std::move(bind), port, std::move(o)));
}

UdpReceiver::UdpReceiver(asio::io_context& io, std::string bind, uint16_t port, UdpReceiveOptions o)
    : impl_(std::make_unique<Impl>(io, std::move(bind), port, std::move(o)))
{
    auto& m = *impl_;
    boost::system::error_code ec;
    if (!m.opts.peer.empty()) {
        m.peer_addr = unmapV4(asio::ip::make_address(m.opts.peer, ec));
        if (ec)
            throw std::invalid_argument("udp receiver peer is not an IP address: " + m.opts.peer);
        m.has_peer_filter = true;
    }
    if (!m.opts.group.empty()) {
        m.group_addr = asio::ip::make_address(m.opts.group, ec);
        if (ec || !isMulticast(m.group_addr))
            throw std::invalid_argument("udp receiver group is not a multicast address: " + m.opts.group);
    }
}

UdpReceiver::~UdpReceiver() = default;

void UdpReceiver::onDatagram(std::function<void(const uint8_t*, std::size_t, const udp::endpoint&)> h)
{
    impl_->on_datagram = std::move(h);
}
bool UdpReceiver::bound() const { return impl_->up.load(); }
uint16_t UdpReceiver::localPort() const { return impl_->bound_port.load(); }
const Counters& UdpReceiver::counters() const { return impl_->counters; }
int UdpReceiver::receiveBufferBytes() const { return impl_->rcvbuf.load(); }
void UdpReceiver::onState(std::function<void(bool)> h) { impl_->on_state = std::move(h); }

// Posted, never called from inside start(), stop() or a receive handler.
void UdpReceiver::notifyState(bool bound)
{
    auto& m = *impl_;
    if (!m.on_state)
        return;
    std::weak_ptr<UdpReceiver> weak = shared_from_this();
    const uint64_t epoch = m.stop_epoch;
    asio::post(m.io, [weak, epoch, bound] {
        auto self = weak.lock();
        if (self && self->impl_->stop_epoch == epoch && self->impl_->on_state)
            self->impl_->on_state(bound);
    });
}
int UdpReceiver::nativeHandleForTest() const
{
    const auto& rx = impl_->rx;
    return rx && rx->sock.is_open() ? static_cast<int>(rx->sock.native_handle()) : -1;
}

void UdpReceiver::start()
{
    auto& m = *impl_;
    if (m.running)
        return;
    m.running = true;
    ++m.gen;
    m.failed_before = false;
    bindAndJoin();
}

void UdpReceiver::stop()
{
    auto& m = *impl_;
    m.running = false;
    ++m.gen;
    m.rebind.cancel();
    ++m.stop_epoch;
    boost::system::error_code ec;
    if (m.rx)
        m.rx->sock.close(ec);
    m.rx.reset(); // handlers in flight keep their own reference
    m.up = false;
    m.rcvbuf = 0;
}

void UdpReceiver::fail(const std::string& why)
{
    auto& m = *impl_;
    if (m.rx) {
        boost::system::error_code ignore;
        m.rx->sock.close(ignore);
        m.rx.reset();
    }
    const bool was_up = m.up.exchange(false);
    m.rcvbuf = 0;
    m.counters.last_error = why;
    log(m.failed_before ? LogLevel::debug : LogLevel::warn, m.opts.name, why);
    m.failed_before = true;
    if (was_up)
        notifyState(false);
    std::weak_ptr<UdpReceiver> weak = shared_from_this();
    const uint64_t gen = m.gen;
    m.rebind.after(m.opts.rebind_delay, [weak, gen] {
        auto self = weak.lock();
        if (self && self->impl_->running && self->impl_->gen == gen)
            self->bindAndJoin();
    });
}

void UdpReceiver::bindAndJoin()
{
    auto& m = *impl_;
    const bool multicast = !m.opts.group.empty();
    auto rx = std::make_shared<Rx>(m.io);
    boost::system::error_code ec;
    std::string where;

    asio::ip::address bindAddr;
    if (multicast) {
        // v4: the group address, so only that group's traffic arrives; v6: the wildcard.
        bindAddr = m.group_addr.is_v4() ? m.group_addr : asio::ip::address(asio::ip::address_v6::any());
    } else {
        bindAddr = asio::ip::make_address(m.bind.empty() ? "0.0.0.0" : m.bind, ec);
        if (ec) {
            fail("bind address " + m.bind + ": " + ec.message());
            return;
        }
    }
    where = bindAddr.to_string() + ":" + std::to_string(m.port);

    rx->sock.open(bindAddr.is_v6() ? udp::v6() : udp::v4(), ec);
    if (ec) {
        fail("open " + where + ": " + ec.message());
        return;
    }
    // Best effort, before bind: a burst must not overrun the kernel default
    // while a slow consumer catches up.
    {
        boost::system::error_code ignore;
        rx->sock.set_option(asio::socket_base::receive_buffer_size(static_cast<int>(m.opts.receive_buffer)), ignore);
        asio::socket_base::receive_buffer_size got;
        rx->sock.get_option(got, ignore);
        m.rcvbuf = ignore ? 0 : got.value();
        if (!m.failed_before)
            log(LogLevel::debug, m.opts.name, "udp " + where + ": receive buffer " + std::to_string(ignore ? 0 : got.value()) +
                                    " bytes");
    }
    if (multicast) {
        rx->sock.set_option(asio::socket_base::reuse_address(true), ec);
        if (ec) {
            fail("reuse_address " + where + ": " + ec.message());
            return;
        }
    }
    rx->sock.bind(udp::endpoint(bindAddr, m.port), ec);
    if (ec) {
        fail("bind " + where + ": " + ec.message());
        return;
    }
    if (multicast) {
        const std::string via = m.opts.iface.empty() ? "default interface" : m.opts.iface;
        if (m.group_addr.is_v4()) {
            asio::ip::address_v4 iface = asio::ip::address_v4::any();
            if (!m.opts.iface.empty()) {
                const auto a = asio::ip::make_address(m.opts.iface, ec);
                if (!ec && a.is_v4())
                    iface = a.to_v4();
                else
                    ec = asio::error::invalid_argument;
            }
            if (!ec)
                rx->sock.set_option(asio::ip::multicast::join_group(m.group_addr.to_v4(), iface), ec);
        } else {
            const unsigned idx = interfaceIndex(m.opts.iface);
            if (!m.opts.iface.empty() && idx == 0)
                ec = asio::error::invalid_argument;
            else
                rx->sock.set_option(asio::ip::multicast::join_group(m.group_addr.to_v6(), idx), ec);
        }
        if (ec) {
            fail("join " + m.opts.group + " on " + via + ": " + ec.message());
            return;
        }
    }

    m.rx = rx;
    m.bound_port = rx->sock.local_endpoint(ec).port();
    m.failed_before = false;
    m.counters.last_error.clear();
    m.up = true;
    log(LogLevel::debug, m.opts.name, "udp receiving on " + where + (multicast ? " group " + m.opts.group : std::string()));
    notifyState(true);
    receiveLoop(m.gen, rx);
}

void UdpReceiver::receiveLoop(uint64_t gen, const std::shared_ptr<Rx>& rx)
{
    auto self = shared_from_this();
    rx->sock.async_receive_from(asio::buffer(rx->buf), rx->from,
                                [self, gen, rx](const boost::system::error_code& ec, std::size_t n) {
        auto& m = *self->impl_;
        if (gen != m.gen)
            return;
        if (ec) {
            if (ec == asio::error::operation_aborted)
                return;
            self->fail("receive: " + ec.message());
            return;
        }
        const auto from = rx->from;
        if (m.has_peer_filter && unmapV4(from.address()) != m.peer_addr) {
            ++m.counters.rejected_peer;
            // Named in last_error at most once a second.
            const auto now = std::chrono::steady_clock::now();
            if (!m.peer_noted || now - *m.peer_noted >= std::chrono::seconds(1)) {
                m.peer_noted = now;
                m.counters.last_error = "datagram from " + from.address().to_string() + " refused (not the configured peer)";
            }
        } else {
            ++m.counters.messages_in;
            m.counters.bytes_in += n;
            if (m.on_datagram)
                m.on_datagram(rx->buf.data(), n, from);
        }
        if (gen != m.gen)
            return; // a callback stopped us
        self->receiveLoop(gen, rx);
    });
}

} // namespace skycue::net
