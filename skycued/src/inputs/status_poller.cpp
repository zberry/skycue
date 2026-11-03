#include "inputs/status_poller.hpp"

// Boost.Beast as of Boost 1.66 (the oldest supported): http::async_write of
// a request, http::async_read_header and http::async_read into a
// response_parser with body_limit.
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <string>
#include <utility>

namespace skycued {

namespace asio = boost::asio;
namespace http = boost::beast::http;
using asio::ip::tcp;
using boost::system::error_code;

namespace {

// One request. Its handlers hold it, so the socket, buffer and parser outlive
// every operation on them, even after stop() dropped it.
struct Attempt {
    explicit Attempt(asio::io_context& io) : resolver(io), sock(io), timer(io) {}

    void abort()
    {
        error_code ignored;
        resolver.cancel();
        sock.close(ignored);
        timer.cancel(ignored);
    }

    tcp::resolver resolver;
    tcp::socket sock;
    asio::steady_timer timer;  // the timeout
    http::request<http::empty_body> req;
    boost::beast::flat_buffer buf;
    http::response_parser<http::string_body> parser;
    bool timed_out = false;
};

}  // namespace

struct StatusPoller::Impl : std::enable_shared_from_this<Impl> {
    Impl(asio::io_context& io_, HttpUrl url_, std::chrono::seconds interval_, std::chrono::milliseconds timeout_)
        : io(io_), url(std::move(url_)), interval(interval_), timeout(timeout_), next(io_)
    {
    }

    asio::io_context& io;
    HttpUrl url;
    std::chrono::seconds interval;
    std::chrono::milliseconds timeout;
    ResultHandler handler;
    asio::steady_timer next;  // the next request
    bool running = false;
    unsigned gen = 0;  // bumped by stop(): a wait armed before it is stale
    std::shared_ptr<Attempt> attempt;  // the request in flight
    std::chrono::steady_clock::time_point last_start;

    std::string where() const { return url.host + ":" + std::to_string(url.port); }

    void poll()
    {
        last_start = std::chrono::steady_clock::now();
        auto a = std::make_shared<Attempt>(io);
        attempt = a;
        a->parser.body_limit(kMaxStatusBytes);
        a->req.method(http::verb::get);
        a->req.target(url.target);
        a->req.version(11);
        a->req.set(http::field::host, url.port == 80 ? url.host : where());
        a->req.set(http::field::connection, "close");

        std::weak_ptr<Attempt> weak_attempt = a;
        a->timer.expires_after(timeout);
        a->timer.async_wait([weak_attempt](const error_code& ec) {
            auto at = weak_attempt.lock();
            if (ec || !at)
                return;
            at->timed_out = true;
            at->abort();  // the pending operation fails and finishes the attempt
        });

        std::weak_ptr<Impl> weak = shared_from_this();
        a->resolver.async_resolve(url.host, std::to_string(url.port),
                                  [weak, a](const error_code& ec, tcp::resolver::results_type results) {
                                      auto self = weak.lock();
                                      if (!self || self->attempt != a)
                                          return;
                                      if (ec) {
                                          self->finish(a, nullptr, "cannot resolve " + self->url.host + ": " +
                                                                       ec.message());
                                          return;
                                      }
                                      self->connect(a, results);
                                  });
    }

    void connect(const std::shared_ptr<Attempt>& a, const tcp::resolver::results_type& results)
    {
        std::weak_ptr<Impl> weak = shared_from_this();
        asio::async_connect(a->sock, results, [weak, a](const error_code& ec, const tcp::endpoint&) {
            auto self = weak.lock();
            if (!self || self->attempt != a)
                return;
            if (ec) {
                self->finish(a, nullptr, "cannot connect to " + self->where() + ": " + ec.message());
                return;
            }
            self->send(a);
        });
    }

    void send(const std::shared_ptr<Attempt>& a)
    {
        std::weak_ptr<Impl> weak = shared_from_this();
        http::async_write(a->sock, a->req, [weak, a](const error_code& ec, std::size_t) {
            auto self = weak.lock();
            if (!self || self->attempt != a)
                return;
            if (ec) {
                self->finish(a, nullptr, "cannot send to " + self->where() + ": " + ec.message());
                return;
            }
            self->receive(a);
        });
    }

    // The header first: an eager read (http::async_read) can lose the
    // body_limit error of a Content-Length over the limit (seen in Boost
    // 1.75), a header-only read reports it.
    void receive(const std::shared_ptr<Attempt>& a)
    {
        std::weak_ptr<Impl> weak = shared_from_this();
        http::async_read_header(a->sock, a->buf, a->parser, [weak, a](const error_code& ec, std::size_t) {
            auto self = weak.lock();
            if (!self || self->attempt != a)
                return;
            if (ec || a->parser.is_done()) {
                self->received(a, ec);
                return;
            }
            http::async_read(a->sock, a->buf, a->parser, [weak, a](const error_code& bec, std::size_t) {
                auto s = weak.lock();
                if (s && s->attempt == a)
                    s->received(a, bec);
            });
        });
    }

    void received(const std::shared_ptr<Attempt>& a, const error_code& ec)
    {
        if (ec == http::error::body_limit) {
            finish(a, nullptr, "response body over " + std::to_string(kMaxStatusBytes) + " bytes");
            return;
        }
        if (ec) {
            finish(a, nullptr, "cannot read the response from " + where() + ": " + ec.message());
            return;
        }
        const unsigned code = a->parser.get().result_int();
        if (code == 200) {
            finish(a, &a->parser.get().body(), std::string());
            return;
        }
        std::string error = "HTTP " + std::to_string(code);
        if (code >= 300 && code < 400)
            error += " (redirects are not followed)";
        finish(a, nullptr, error);
    }

    // Ends the attempt, arms the next one and reports.
    void finish(const std::shared_ptr<Attempt>& a, const std::string* body, std::string error)
    {
        if (!body && a->timed_out)
            error = "no response within " + std::to_string(timeout.count()) + " ms";
        a->abort();
        attempt.reset();
        armNext();  // before the handler, so a stop() from it cancels the wait
        if (auto h = handler)  // a copy: the handler may replace itself
            h(body, error);
    }

    void armNext()
    {
        next.expires_at(last_start + interval);
        std::weak_ptr<Impl> weak = shared_from_this();
        next.async_wait([weak, g = gen](const error_code& ec) {
            auto self = weak.lock();
            if (ec || !self || !self->running || self->gen != g || self->attempt)
                return;
            self->poll();
        });
    }

    void stop()
    {
        if (!running)
            return;
        running = false;
        ++gen;
        error_code ignored;
        next.cancel(ignored);
        if (attempt) {
            attempt->abort();
            attempt.reset();
        }
    }
};

StatusPoller::StatusPoller(asio::io_context& io, HttpUrl url, std::chrono::seconds interval,
                           std::chrono::milliseconds timeout)
    : impl_(std::make_shared<Impl>(io, std::move(url), interval, timeout))
{
}

StatusPoller::~StatusPoller() { impl_->stop(); }

void StatusPoller::onResult(ResultHandler handler) { impl_->handler = std::move(handler); }

void StatusPoller::start()
{
    if (impl_->running)
        return;
    impl_->running = true;
    impl_->poll();
}

void StatusPoller::stop() { impl_->stop(); }

}  // namespace skycued
