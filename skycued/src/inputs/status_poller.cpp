#include "inputs/status_poller.hpp"

// Boost.Beast as of Boost 1.66 (the oldest supported): http::async_write of
// a request, http::async_read_header and http::async_read into a
// response_parser with body_limit.
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
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
        timer.cancel();
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

std::string httpHostHeader(const HttpUrl& url)
{
    std::string host = url.host.find(':') != std::string::npos ? "[" + url.host + "]" : url.host;
    if (url.port != 80)
        host += ":" + std::to_string(url.port);
    return host;
}

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
    // Resolves issued and not yet completed. Asio runs every resolve of the
    // io_context on one private thread, and cancel() cannot interrupt a
    // getaddrinfo that blocks (a silent nameserver), so a timed-out attempt
    // can leave its lookup behind. A poll is skipped while one is.
    unsigned resolves_pending = 0;

    std::string where() const { return url.host + ":" + std::to_string(url.port); }

    // The poller when `a` is still its request in flight, else null. Every
    // completion handler starts here. A timed-out attempt is finished here:
    // a completion queued before the timeout fired must not go on (a late
    // resolve would let async_connect reopen the closed socket, with no
    // timer left to end the attempt).
    static std::shared_ptr<Impl> live(const std::weak_ptr<Impl>& weak, const std::shared_ptr<Attempt>& a)
    {
        auto self = weak.lock();
        if (!self || self->attempt != a)
            return nullptr;
        if (a->timed_out) {
            self->finish(a, nullptr, std::string());
            return nullptr;
        }
        return self;
    }

    void poll()
    {
        last_start = std::chrono::steady_clock::now();
        if (resolves_pending > 0) {
            skip("resolve still pending");
            return;
        }
        auto a = std::make_shared<Attempt>(io);
        attempt = a;
        a->parser.body_limit(kMaxStatusBytes);
        a->req.method(http::verb::get);
        a->req.target(url.target);
        a->req.version(11);
        a->req.set(http::field::host, httpHostHeader(url));
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
        ++resolves_pending;
        a->resolver.async_resolve(url.host, std::to_string(url.port),
                                  [weak, a](const error_code& ec, tcp::resolver::results_type results) {
                                      if (auto p = weak.lock())
                                          --p->resolves_pending;
                                      auto self = live(weak, a);
                                      if (!self)
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
            auto self = live(weak, a);
            if (!self)
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
            auto self = live(weak, a);
            if (!self)
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
            auto self = live(weak, a);
            if (!self)
                return;
            // Not a 200: the status code decides and the body is not read.
            if (ec || a->parser.is_done() || a->parser.get().result_int() != 200) {
                self->received(a, ec);
                return;
            }
            http::async_read(a->sock, a->buf, a->parser, [weak, a](const error_code& bec, std::size_t) {
                if (auto s = live(weak, a))
                    s->received(a, bec);
            });
        });
    }

    void received(const std::shared_ptr<Attempt>& a, const error_code& ec)
    {
        // The parsed status line decides first (an unparsed one reads as the
        // default 200), so a 500 announcing a large body is "HTTP 500".
        const unsigned code = a->parser.get().result_int();
        if (code != 200) {
            std::string error = "HTTP " + std::to_string(code);
            if (code >= 300 && code < 400)
                error += " (redirects are not followed)";
            finish(a, nullptr, error);
            return;
        }
        if (ec == http::error::body_limit) {
            finish(a, nullptr, "response body over " + std::to_string(kMaxStatusBytes) + " bytes");
            return;
        }
        if (ec) {
            finish(a, nullptr, "cannot read the response from " + where() + ": " + ec.message());
            return;
        }
        finish(a, &a->parser.get().body(), std::string());
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

    // A poll not attempted: reported as a failure (from the io_context, not
    // from inside start()), and the next one armed as usual.
    void skip(const char* why)
    {
        armNext();
        std::weak_ptr<Impl> weak = shared_from_this();
        asio::post(io, [weak, g = gen, why] {
            auto self = weak.lock();
            if (!self || !self->running || self->gen != g)
                return;
            if (auto h = self->handler)
                h(nullptr, why);
        });
    }

    void armNext()
    {
        // Counted from the last start, but never sooner than min(interval,
        // 1 s) after this finish, so an overrunning request is not followed
        // back to back. (InputDef keeps status_interval_s in 5..3600, so an
        // interval of 0 does not reach here from an input.)
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::steady_clock::duration gap = std::min<std::chrono::steady_clock::duration>(
            interval, std::chrono::seconds(1));
        next.expires_at(std::max(last_start + interval, now + gap));
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
        next.cancel();
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
