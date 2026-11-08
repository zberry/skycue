#include "inputs/tcp_connect_client.hpp"

#include "skycue/net/dialer.hpp"

#include <utility>

namespace skycued {

namespace asio = boost::asio;
namespace net = skycue::net;

struct TcpConnectClient::Impl : TransportState, std::enable_shared_from_this<Impl> {
    Impl(asio::io_context& io, std::string host, uint16_t port, std::string name)
    {
        net::TcpOptions o;
        o.keepalive = true;
        o.nodelay = false;  // this side only reads
        o.chunk_bytes = kChunkBytes;
        o.name = std::move(name);
        boost::system::error_code ec;
        remote = asio::ip::make_address(host, ec);  // unspecified when the host is a name
        where = host + ":" + std::to_string(port);
        resolve_prefix = "resolve " + host + ": ";
        dialer = net::Dialer::create(io, std::move(host), port, o);
    }

    std::shared_ptr<net::Dialer> dialer;
    asio::ip::address remote;
    uint64_t stream = 0;  // the connection that is up (or was last)
    std::string lib_error_seen;
    std::string where, resolve_prefix;

    void wire()
    {
        std::weak_ptr<Impl> weak = shared_from_this();
        dialer->onData([weak](const uint8_t* data, std::size_t n) {
            auto self = weak.lock();
            if (!self || self->stopping)
                return;
            if (self->on_data)
                self->on_data(TransportData{data, n, self->stream, self->remote});
        });
        dialer->onConnection([weak](bool up) {
            auto self = weak.lock();
            if (!self || self->stopping)
                return;
            if (up) {
                self->stream = self->dialer->counters().connects;
            } else if (self->on_stream_closed) {
                self->on_stream_closed(self->stream);
                if (self->stopping)
                    return;
            }
            if (self->on_conn)
                self->on_conn(up);
        });
        sync = [weak] {
            if (auto self = weak.lock())
                self->copyCounters();
        };
    }

    void copyCounters()
    {
        const auto& c = dialer->counters();
        bytes = c.bytes_in;
        connects = c.connects;
        peers = dialer->connected() ? 1 : 0;
        // Sticky, as it always was: a later success does not clear it.
        if (!c.last_error.empty() && c.last_error != lib_error_seen) {
            lib_error_seen = c.last_error;
            last_error = oldWording(c.last_error);
        }
    }

    // The library's reasons in the words this transport has always used:
    // "resolve host:port: ...", "connect host:port: ...", "disconnected from host:port: ...".
    std::string oldWording(const std::string& e) const
    {
        if (e.compare(0, resolve_prefix.size(), resolve_prefix) == 0)
            return "resolve " + where + ": " + e.substr(resolve_prefix.size());
        for (const char* p : {"read: ", "write: "})
            if (e.compare(0, std::char_traits<char>::length(p), p) == 0)
                return "disconnected from " + where + ": " + e.substr(std::char_traits<char>::length(p));
        return e;
    }

    void shutdown()
    {
        if (!stopping)
            copyCounters();
        stopping = true;
        dialer->stop();
        peers = 0;
    }
};

TcpConnectClient::TcpConnectClient(asio::io_context& io, std::string host, uint16_t port, std::string name)
    : impl_(std::make_shared<Impl>(io, std::move(host), port, std::move(name)))
{
    state_ = impl_;
    impl_->wire();
}

TcpConnectClient::~TcpConnectClient() { impl_->shutdown(); }

void TcpConnectClient::start()
{
    if (impl_->started || impl_->stopping)
        return;
    impl_->started = true;
    impl_->dialer->start();
}

void TcpConnectClient::stop() { impl_->shutdown(); }

bool TcpConnectClient::connected() const { return !impl_->stopping && impl_->dialer->connected(); }

}  // namespace skycued
