#include "inputs/udp_listener.hpp"

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <optional>
#include <utility>
#include <vector>

namespace skycued {

namespace asio = boost::asio;
using asio::ip::udp;
using boost::system::error_code;

struct UdpListener::Impl : TransportState, std::enable_shared_from_this<Impl> {
    Impl(asio::io_context& io_, std::string bind_, uint16_t port_, std::string peer_,
         std::chrono::milliseconds rebind_delay_)
        : io(io_), sock(io_), timer(io_), bind(std::move(bind_)), port(port_), peer(parsePeerFilter(peer_)),
          rebind_delay(rebind_delay_), buf(kBufferBytes)
    {
    }

    asio::io_context& io;
    udp::socket sock;
    asio::steady_timer timer;
    std::string bind;
    uint16_t port;
    std::optional<asio::ip::address> peer;
    std::chrono::milliseconds rebind_delay;
    std::vector<uint8_t> buf;
    udp::endpoint sender;
    bool bound = false;
    // When last_error last named a refused peer (at most once a second).
    std::optional<std::chrono::steady_clock::time_point> peer_noted;

    void open()
    {
        if (stopping)
            return;
        error_code ec;
        const auto addr = asio::ip::make_address(bind, ec);
        if (!ec)
            sock.open(addr.is_v6() ? udp::v6() : udp::v4(), ec);
        if (!ec)
            sock.bind(udp::endpoint(addr, port), ec);
        if (ec) {
            error_code ignored;
            sock.close(ignored);
            last_error = "bind " + bind + ":" + std::to_string(port) + ": " + ec.message();
            retryLater();
            return;
        }
        bound = true;
        ++connects;
        if (on_conn)
            on_conn(true);
        if (!stopping)
            receive();
    }

    void retryLater()
    {
        timer.expires_after(rebind_delay);
        std::weak_ptr<Impl> weak = shared_from_this();
        timer.async_wait([weak](const error_code& ec) {
            auto self = weak.lock();
            if (!ec && self && !self->stopping)
                self->open();
        });
    }

    void receive()
    {
        std::weak_ptr<Impl> weak = shared_from_this();
        sock.async_receive_from(asio::buffer(buf), sender, [weak](const error_code& ec, std::size_t n) {
            auto self = weak.lock();
            if (!self || self->stopping)
                return;
            if (ec) {
                self->failed(ec);
                return;
            }
            self->deliver(n);
            if (!self->stopping)
                self->receive();
        });
    }

    void deliver(std::size_t n)
    {
        const auto from = sender.address();
        if (peer && from != *peer) {
            ++rejected_peer;
            const auto now = std::chrono::steady_clock::now();
            if (!peer_noted || now - *peer_noted >= std::chrono::seconds(1)) {
                peer_noted = now;
                last_error = "datagram from " + from.to_string() + " refused (not the configured peer)";
            }
            return;
        }
        bytes += n;
        if (on_data)
            on_data(TransportData{buf.data(), n, 0, from});
    }

    // A receive error closes the socket and binds again after the delay.
    void failed(const error_code& ec)
    {
        last_error = "receive: " + ec.message();
        error_code ignored;
        sock.close(ignored);
        bound = false;
        if (on_conn)
            on_conn(false);
        if (!stopping)
            retryLater();
    }

    void shutdown()
    {
        stopping = true;
        bound = false;
        error_code ignored;
        timer.cancel(ignored);
        sock.close(ignored);
    }
};

UdpListener::UdpListener(asio::io_context& io, std::string bind, uint16_t port, std::string peer,
                         std::chrono::milliseconds rebind_delay)
    : impl_(std::make_shared<Impl>(io, std::move(bind), port, std::move(peer), rebind_delay))
{
    state_ = impl_;
}

UdpListener::~UdpListener() { impl_->shutdown(); }

void UdpListener::start()
{
    if (impl_->started || impl_->stopping)
        return;
    impl_->started = true;
    // Bound from the io_context, so onConnection never runs inside start().
    std::weak_ptr<Impl> weak = impl_;
    asio::post(impl_->io, [weak] {
        if (auto self = weak.lock())
            self->open();
    });
}

void UdpListener::stop() { impl_->shutdown(); }

bool UdpListener::connected() const { return impl_->bound; }

}  // namespace skycued
