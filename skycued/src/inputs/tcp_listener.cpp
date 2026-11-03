#include "inputs/tcp_listener.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <array>
#include <map>
#include <utility>

namespace skycued {

namespace asio = boost::asio;
using asio::ip::tcp;
using boost::system::error_code;

namespace {
struct Session {
    explicit Session(asio::io_context& io) : sock(io) {}
    tcp::socket sock;
    asio::ip::address remote;
    std::array<uint8_t, TcpListener::kChunkBytes> buf{};
};
}  // namespace

struct TcpListener::Impl : TransportState, std::enable_shared_from_this<Impl> {
    Impl(asio::io_context& io_, std::string bind_, uint16_t port_, const std::string& peer_,
         std::chrono::milliseconds rebind_delay_)
        : io(io_), acceptor(io_), timer(io_), bind(std::move(bind_)), port(port_), peer(parsePeerFilter(peer_)),
          rebind_delay(rebind_delay_)
    {
    }

    asio::io_context& io;
    tcp::acceptor acceptor;
    asio::steady_timer timer;
    std::string bind;
    uint16_t port;
    std::optional<asio::ip::address> peer;
    std::chrono::milliseconds rebind_delay;
    // Sessions own their socket and buffer; read handlers hold them strongly.
    std::map<uint64_t, std::shared_ptr<Session>> sessions;
    uint64_t next_stream = 0;

    void open()
    {
        if (stopping)
            return;
        error_code ec;
        const auto addr = asio::ip::make_address(bind, ec);
        if (!ec)
            acceptor.open(addr.is_v6() ? tcp::v6() : tcp::v4(), ec);
        // Only lets the port be reused past TIME_WAIT (after a restart with
        // live peers); on Linux a second listener on the port still fails.
        if (!ec)
            acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
        if (!ec)
            acceptor.bind(tcp::endpoint(addr, port), ec);
        if (!ec)
            acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            error_code ignored;
            acceptor.close(ignored);
            last_error = "bind " + bind + ":" + std::to_string(port) + ": " + ec.message();
            later([](Impl& self) { self.open(); });
            return;
        }
        accept();
    }

    template <class F> void later(F f)
    {
        timer.expires_after(rebind_delay);
        std::weak_ptr<Impl> weak = shared_from_this();
        timer.async_wait([weak, f](const error_code& ec) {
            auto self = weak.lock();
            if (!ec && self && !self->stopping)
                f(*self);
        });
    }

    void accept()
    {
        auto s = std::make_shared<Session>(io);
        std::weak_ptr<Impl> weak = shared_from_this();
        acceptor.async_accept(s->sock, [weak, s](const error_code& ec) {
            auto self = weak.lock();
            if (!self || self->stopping)
                return;
            if (ec) {
                // EMFILE and the like: wait instead of spinning.
                self->last_error = "accept: " + ec.message();
                self->later([](Impl& me) { me.accept(); });
                return;
            }
            self->admit(s);
            if (!self->stopping)
                self->accept();
        });
    }

    void admit(const std::shared_ptr<Session>& s)
    {
        error_code ec;
        const auto ep = s->sock.remote_endpoint(ec);
        if (ec)
            return;  // already gone
        s->remote = ep.address();
        if (peer && s->remote != *peer) {
            ++rejected_peer;
            last_error = "refused a connection from " + s->remote.to_string() + " (not the configured peer)";
            s->sock.close(ec);
            return;
        }
        if (sessions.size() >= kMaxPeers) {
            ++rejected_peer;
            last_error = "refused a connection from " + s->remote.to_string() + ": already " +
                         std::to_string(kMaxPeers) + " peers";
            s->sock.close(ec);
            return;
        }
        enableKeepalive(s->sock);
        const uint64_t id = ++next_stream;
        sessions.emplace(id, s);
        peers = sessions.size();
        ++connects;
        if (peers == 1 && on_conn)
            on_conn(true);
        if (!stopping)
            read(id, s);
    }

    void read(uint64_t id, const std::shared_ptr<Session>& s)
    {
        std::weak_ptr<Impl> weak = shared_from_this();
        s->sock.async_read_some(asio::buffer(s->buf), [weak, id, s](const error_code& ec, std::size_t n) {
            auto self = weak.lock();
            if (!self || self->stopping)
                return;
            if (n > 0) {
                self->bytes += n;
                if (self->on_data)
                    self->on_data(TransportData{s->buf.data(), n, id, s->remote});
                if (self->stopping)
                    return;
            }
            if (ec) {
                self->closed(id);
                return;
            }
            self->read(id, s);
        });
    }

    void closed(uint64_t id)
    {
        sessions.erase(id);
        peers = sessions.size();
        if (on_stream_closed)
            on_stream_closed(id);
        if (!stopping && peers == 0 && on_conn)
            on_conn(false);
    }

    void shutdown()
    {
        stopping = true;
        error_code ignored;
        timer.cancel(ignored);
        acceptor.close(ignored);
        for (auto& s : sessions)
            s.second->sock.close(ignored);
        sessions.clear();
        peers = 0;
    }
};

TcpListener::TcpListener(asio::io_context& io, std::string bind, uint16_t port, const std::string& peer,
                         std::chrono::milliseconds rebind_delay)
    : impl_(std::make_shared<Impl>(io, std::move(bind), port, peer, rebind_delay))
{
    state_ = impl_;
}

TcpListener::~TcpListener() { impl_->shutdown(); }

void TcpListener::start()
{
    if (impl_->started || impl_->stopping)
        return;
    impl_->started = true;
    std::weak_ptr<Impl> weak = impl_;
    asio::post(impl_->io, [weak] {
        if (auto self = weak.lock())
            self->open();
    });
}

void TcpListener::stop() { impl_->shutdown(); }

bool TcpListener::connected() const { return impl_->peers > 0; }

}  // namespace skycued
