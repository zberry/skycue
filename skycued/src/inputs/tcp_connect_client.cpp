#include "inputs/tcp_connect_client.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace skycued {

namespace asio = boost::asio;
using asio::ip::tcp;
using boost::system::error_code;

namespace {
// The LineClient backoff (line_client.cpp); keep the two in step.
constexpr std::chrono::milliseconds kMinBackoff{500};
constexpr std::chrono::milliseconds kMaxBackoff{10000};
}  // namespace

struct TcpConnectClient::Impl : TransportState, std::enable_shared_from_this<Impl> {
    Impl(asio::io_context& io_, std::string host_, uint16_t port_)
        : resolver(io_), sock(io_), timer(io_), host(std::move(host_)), port(port_)
    {
    }

    tcp::resolver resolver;
    tcp::socket sock;
    asio::steady_timer timer;
    std::string host;
    uint16_t port;
    std::array<uint8_t, kChunkBytes> buf{};
    std::chrono::milliseconds backoff = kMinBackoff;
    bool session = false;
    asio::ip::address remote;

    std::string where() const { return host + ":" + std::to_string(port); }

    void connect()
    {
        if (stopping)
            return;
        std::weak_ptr<Impl> weak = shared_from_this();
        resolver.async_resolve(host, std::to_string(port),
                               [weak](const error_code& ec, tcp::resolver::results_type results) {
                                   auto self = weak.lock();
                                   if (!self || self->stopping)
                                       return;
                                   if (ec) {
                                       self->retry("resolve " + self->where() + ": " + ec.message());
                                       return;
                                   }
                                   asio::async_connect(self->sock, results,
                                                       [weak](const error_code& cec, const tcp::endpoint& ep) {
                                                           auto s = weak.lock();
                                                           if (!s || s->stopping)
                                                               return;
                                                           if (cec) {
                                                               s->retry("connect " + s->where() + ": " + cec.message());
                                                               return;
                                                           }
                                                           s->connectedNow(ep.address());
                                                       });
                               });
    }

    void connectedNow(const asio::ip::address& addr)
    {
        backoff = kMinBackoff;
        enableKeepalive(sock);
        session = true;
        remote = addr;
        peers = 1;
        ++connects;
        if (on_conn)
            on_conn(true);
        if (!stopping)
            read();
    }

    void retry(std::string why)
    {
        last_error = std::move(why);
        error_code ignored;
        sock.close(ignored);
        timer.expires_after(backoff);
        backoff = std::min(backoff * 2, kMaxBackoff);
        std::weak_ptr<Impl> weak = shared_from_this();
        timer.async_wait([weak](const error_code& ec) {
            auto self = weak.lock();
            if (!ec && self && !self->stopping)
                self->connect();
        });
    }

    void read()
    {
        std::weak_ptr<Impl> weak = shared_from_this();
        sock.async_read_some(asio::buffer(buf), [weak](const error_code& ec, std::size_t n) {
            auto self = weak.lock();
            if (!self || self->stopping)
                return;
            if (n > 0) {
                self->bytes += n;
                if (self->on_data)
                    self->on_data(TransportData{self->buf.data(), n, self->connects, self->remote});
                if (self->stopping)
                    return;
            }
            if (ec) {
                self->lost(ec);
                return;
            }
            self->read();
        });
    }

    void lost(const error_code& ec)
    {
        session = false;
        peers = 0;
        if (on_stream_closed)
            on_stream_closed(connects);
        if (stopping)
            return;
        if (on_conn)
            on_conn(false);
        if (stopping)
            return;
        retry("disconnected from " + where() + ": " + ec.message());
    }

    void shutdown()
    {
        stopping = true;
        session = false;
        peers = 0;
        error_code ignored;
        timer.cancel(ignored);
        resolver.cancel();
        sock.close(ignored);
    }
};

TcpConnectClient::TcpConnectClient(asio::io_context& io, std::string host, uint16_t port)
    : impl_(std::make_shared<Impl>(io, std::move(host), port))
{
    state_ = impl_;
}

TcpConnectClient::~TcpConnectClient() { impl_->shutdown(); }

void TcpConnectClient::start()
{
    if (impl_->started || impl_->stopping)
        return;
    impl_->started = true;
    impl_->connect();
}

void TcpConnectClient::stop() { impl_->shutdown(); }

bool TcpConnectClient::connected() const { return impl_->session; }

}  // namespace skycued
