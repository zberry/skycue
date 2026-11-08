#include "line_client.hpp"

#include "log.hpp"

#include "skycue/net/dialer.hpp"
#include "skycue/net/framer.hpp"
#include "skycue/net/tcp_options.hpp"

#include <utility>

namespace skycued {

namespace net = skycue::net;

struct LineClient::Impl {
    Impl(boost::asio::io_context& io, std::string host_, uint16_t port_, std::string name_)
        : host(std::move(host_)), port(port_), name(std::move(name_)), framer(kMaxLineBytes)
    {
        // The socket settings of this client: no keepalive, no nodelay; the
        // reconnect backoff and the read chunk are the library's.
        net::TcpOptions opts;
        opts.keepalive = false;
        opts.nodelay = false;
        opts.name = name;
        dialer = net::Dialer::create(io, host, port, opts);
    }

    std::shared_ptr<net::Dialer> dialer;
    std::string host;
    uint16_t port;
    std::string name;
    net::LineFramer framer;
    std::function<void(const std::string&)> on_line;
    std::function<void(bool)> on_conn;
    bool started = false, stopped = false;
    uint64_t rejected_seen = 0;

    void wire(const std::shared_ptr<Impl>& self)
    {
        std::weak_ptr<Impl> weak = self;
        dialer->onData([weak](const uint8_t* data, std::size_t n) {
            auto me = weak.lock();
            if (!me || me->stopped)
                return;
            me->feed(data, n);
        });
        dialer->onConnection([weak](bool up) {
            auto me = weak.lock();
            if (!me || me->stopped)
                return;
            me->framer.reset();  // a partial line never survives a connection
            if (up) {
                LOG_INFO(me->name << ": connected to " << me->host << ":" << me->port);
            } else {
                // The library's reason reads "read: End of file"; the message was always the bare error text.
                std::string why = me->dialer->counters().last_error;
                if (why.compare(0, 6, "read: ") == 0)
                    why.erase(0, 6);
                LOG_INFO(me->name << ": connection closed (" << why << ")");
            }
            if (me->on_conn)
                me->on_conn(up);
        });
    }

    void feed(const uint8_t* data, std::size_t n)
    {
        framer.feed(data, n, [this](const uint8_t* line, std::size_t len) {
            if (stopped || !on_line)
                return;  // a callback stopped the client: drop the rest of the chunk
            on_line(std::string(reinterpret_cast<const char*>(line), len));
        });
        if (framer.rejected() > rejected_seen) {
            LOG_DEBUG(name << ": discarding a line longer than " << kMaxLineBytes << " bytes");
            rejected_seen = framer.rejected();
        }
    }

    void shutdown()
    {
        stopped = true;
        dialer->stop();
    }
};

LineClient::LineClient(boost::asio::io_context& io, std::string host, uint16_t port, std::string name)
    : impl_(std::make_shared<Impl>(io, std::move(host), port, std::move(name)))
{
    impl_->wire(impl_);
}

LineClient::~LineClient() { impl_->shutdown(); }

void LineClient::onLine(std::function<void(const std::string&)> handler) { impl_->on_line = std::move(handler); }

void LineClient::onConnection(std::function<void(bool)> handler) { impl_->on_conn = std::move(handler); }

void LineClient::start()
{
    if (impl_->started || impl_->stopped)
        return;
    impl_->started = true;
    impl_->dialer->start();
}

void LineClient::stop() { impl_->shutdown(); }

bool LineClient::connected() const { return !impl_->stopped && impl_->dialer->connected(); }

uint64_t LineClient::linesRejected() const { return impl_->framer.rejected(); }

uint64_t LineClient::bytesRead() const { return impl_->dialer->counters().bytes_in; }

uint64_t LineClient::connects() const { return impl_->dialer->counters().connects; }

}  // namespace skycued
