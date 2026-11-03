#include "line_client.hpp"

#include "log.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <utility>

namespace skycued {

namespace asio = boost::asio;
using asio::ip::tcp;
using boost::system::error_code;

namespace {
// inputs/tcp_connect_client.cpp copies this backoff; keep the two in step.
constexpr std::chrono::milliseconds kMinBackoff{500};
constexpr std::chrono::milliseconds kMaxBackoff{10000};
}  // namespace

struct LineClient::Impl : std::enable_shared_from_this<Impl> {
    Impl(asio::io_context& io_, std::string host_, uint16_t port_, std::string name_)
        : resolver(io_), sock(io_), timer(io_), host(std::move(host_)), port(port_), name(std::move(name_))
    {
    }

    tcp::resolver resolver;
    tcp::socket sock;
    asio::steady_timer timer;
    std::string host;
    uint16_t port;
    std::string name;

    std::function<void(const std::string&)> on_line;
    std::function<void(bool)> on_conn;

    std::array<char, 16384> buf{};
    std::string line;
    bool discarding = false;  // inside an oversize line
    std::chrono::milliseconds backoff = kMinBackoff;
    bool started = false, stopping = false, is_connected = false;
    bool quiet = false;  // a failed attempt was already logged at WARN
    unsigned gen = 0;    // bumped per connection attempt; stale handlers compare it
    uint64_t rejected = 0;
    uint64_t bytes_read = 0, connect_count = 0;

    void connect()
    {
        if (stopping)
            return;
        const unsigned g = ++gen;
        auto self = shared_from_this();
        resolver.async_resolve(host, std::to_string(port),
                               [self, g](const error_code& ec, const tcp::resolver::results_type& results) {
                                   if (self->stopping || g != self->gen)
                                       return;
                                   if (ec) {
                                       self->retry("resolve: " + ec.message());
                                       return;
                                   }
                                   asio::async_connect(self->sock, results,
                                                       [self, g](const error_code& cec, const tcp::endpoint&) {
                                                           if (self->stopping || g != self->gen)
                                                               return;
                                                           if (cec) {
                                                               self->retry(cec.message());
                                                               return;
                                                           }
                                                           self->connectedNow(g);
                                                       });
                               });
    }

    void connectedNow(unsigned g)
    {
        LOG_INFO(name << ": connected to " << host << ":" << port);
        backoff = kMinBackoff;
        quiet = false;
        ++connect_count;
        line.clear();
        discarding = false;
        is_connected = true;
        if (on_conn)
            on_conn(true);
        if (!stopping && g == gen)
            read(g);
    }

    void retry(const std::string& why)
    {
        error_code ignored;
        sock.close(ignored);
        if (stopping)
            return;
        if (!quiet)
            LOG_WARN(name << ": " << host << ":" << port << ": " << why << "; retrying in " << backoff.count()
                          << " ms (further failures logged at debug level)");
        else
            LOG_DEBUG(name << ": " << why << "; retrying in " << backoff.count() << " ms");
        quiet = true;
        timer.expires_after(backoff);
        backoff = std::min(backoff * 2, kMaxBackoff);
        const unsigned g = gen;
        auto self = shared_from_this();
        timer.async_wait([self, g](const error_code& ec) {
            if (!ec && !self->stopping && g == self->gen)
                self->connect();
        });
    }

    void read(unsigned g)
    {
        auto self = shared_from_this();
        sock.async_read_some(asio::buffer(buf), [self, g](const error_code& ec, std::size_t n) {
            if (self->stopping || g != self->gen)
                return;
            self->bytes_read += n;
            if (n > 0 && !self->feed(self->buf.data(), n, g))
                return;  // stopped or reconnected from a callback
            if (ec) {
                self->lost(ec);
                return;
            }
            self->read(g);
        });
    }

    void lost(const error_code& ec)
    {
        LOG_INFO(name << ": connection closed (" << ec.message() << ")");
        is_connected = false;
        line.clear();
        discarding = false;
        if (on_conn)
            on_conn(false);
        if (stopping)
            return;
        quiet = false;
        retry("disconnected");
    }

    // Splits data into lines; false when a callback stopped the client.
    bool feed(const char* data, std::size_t n, unsigned g)
    {
        const char* p = data;
        const char* end = data + n;
        while (p < end) {
            const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
            const char* stop_at = nl ? nl : end;
            if (!discarding) {
                const auto len = static_cast<std::size_t>(stop_at - p);
                if (line.size() + len > kMaxLineBytes) {
                    ++rejected;
                    LOG_DEBUG(name << ": discarding a line longer than " << kMaxLineBytes << " bytes");
                    line.clear();
                    discarding = true;
                } else {
                    line.append(p, len);
                }
            }
            if (!nl)
                break;
            p = nl + 1;
            if (discarding) {
                discarding = false;
                continue;
            }
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty()) {
                std::string complete;
                complete.swap(line);
                if (on_line)
                    on_line(complete);
                if (stopping || g != gen)
                    return false;
            }
        }
        return true;
    }

    void shutdown()
    {
        stopping = true;
        ++gen;
        is_connected = false;
        error_code ignored;
        timer.cancel(ignored);
        resolver.cancel();
        sock.close(ignored);
    }
};

LineClient::LineClient(asio::io_context& io, std::string host, uint16_t port, std::string name)
    : impl_(std::make_shared<Impl>(io, std::move(host), port, std::move(name)))
{
}

LineClient::~LineClient() { impl_->shutdown(); }

void LineClient::onLine(std::function<void(const std::string&)> handler) { impl_->on_line = std::move(handler); }

void LineClient::onConnection(std::function<void(bool)> handler) { impl_->on_conn = std::move(handler); }

void LineClient::start()
{
    if (impl_->started || impl_->stopping)
        return;
    impl_->started = true;
    impl_->connect();
}

void LineClient::stop() { impl_->shutdown(); }

bool LineClient::connected() const { return impl_->is_connected; }

uint64_t LineClient::linesRejected() const { return impl_->rejected; }

uint64_t LineClient::bytesRead() const { return impl_->bytes_read; }

uint64_t LineClient::connects() const { return impl_->connect_count; }

}  // namespace skycued
