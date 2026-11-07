#include "skycue/net/dialer.hpp"

#include "skycue/net/backoff.hpp"
#include "skycue/net/log.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <deque>
#include <vector>

namespace skycue::net {

namespace asio = boost::asio;
using asio::ip::tcp;

struct Dialer::Impl {
    Impl(asio::io_context& io_, std::string h, uint16_t p, TcpOptions o)
        : io(io_), host(std::move(h)), port(p), opts(o), resolver(io_), retry(io_),
          backoff(o.min_backoff, o.max_backoff)
    {
    }
    asio::io_context& io;
    std::string host;
    uint16_t port;
    TcpOptions opts;
    tcp::resolver resolver;
    RetryTimer retry;
    Backoff backoff;
    std::shared_ptr<tcp::socket> socket; // handlers of composed operations hold it too, so a reset is safe
    uint64_t stop_epoch = 0; // changes on stop(); suppresses notifications posted earlier
    std::vector<uint8_t> rbuf;
    std::function<void(const uint8_t*, std::size_t)> on_data;
    std::function<void(bool)> on_conn;
    Counters counters;
    uint64_t attempts = 0;
    uint64_t gen = 0;       // changes on every connection attempt and on stop()
    bool running = false;
    bool failed_before = false; // a failure since the last success was already logged at warn
    std::atomic<bool> up{false};
    std::deque<Buf> queue;  // the first `in_flight` entries are being written
    std::size_t in_flight = 0;
    std::size_t queued_bytes = 0;
};

std::shared_ptr<Dialer> Dialer::create(asio::io_context& io, std::string host, uint16_t port, TcpOptions o)
{
    return std::shared_ptr<Dialer>(new Dialer(io, std::move(host), port, o));
}

Dialer::Dialer(asio::io_context& io, std::string host, uint16_t port, TcpOptions o)
    : impl_(std::make_unique<Impl>(io, std::move(host), port, o))
{
}

Dialer::~Dialer() = default;

void Dialer::onData(std::function<void(const uint8_t*, std::size_t)> h) { impl_->on_data = std::move(h); }
void Dialer::onConnection(std::function<void(bool)> h) { impl_->on_conn = std::move(h); }
bool Dialer::connected() const { return impl_->up.load(); }
const Counters& Dialer::counters() const { return impl_->counters; }
uint64_t Dialer::attempts() const { return impl_->attempts; }

void Dialer::start()
{
    auto& m = *impl_;
    if (m.running)
        return;
    m.running = true;
    m.failed_before = false;
    m.backoff.reset();
    connect();
}

void Dialer::stop()
{
    auto& m = *impl_;
    m.running = false;
    ++m.gen;
    ++m.stop_epoch;
    m.retry.cancel();
    boost::system::error_code ec;
    m.resolver.cancel();
    if (m.socket)
        m.socket->close(ec);
    m.socket.reset();
    m.up = false;
    m.queue.clear();
    m.in_flight = 0;
    m.queued_bytes = 0;
}

void Dialer::connect()
{
    auto& im = *impl_;
    const uint64_t gen = ++im.gen;
    ++im.attempts;
    im.socket = std::make_shared<tcp::socket>(im.io);
    auto sock = im.socket;
    auto self = shared_from_this();
    im.resolver.async_resolve(im.host, std::to_string(im.port),
                             [self, gen, sock](const boost::system::error_code& ec, const tcp::resolver::results_type& results) {
        auto& m = *self->impl_;
        if (gen != m.gen)
            return;
        if (ec) {
            self->lost(gen, "resolve " + m.host + ": " + ec.message());
            return;
        }
        asio::async_connect(*sock, results,
                            [self, gen, sock](const boost::system::error_code& cec, const tcp::endpoint&) {
            auto& st = *self->impl_;
            if (gen != st.gen)
                return;
            if (cec) {
                self->lost(gen, "connect " + st.host + ":" + std::to_string(st.port) + ": " + cec.message());
                return;
            }
            applyTcpOptions(*sock, st.opts);
            st.backoff.reset();
            st.failed_before = false;
            ++st.counters.connects;
            st.counters.last_error.clear();
            st.up = true;
            log(LogLevel::info, "connected to " + st.host + ":" + std::to_string(st.port));
            if (st.on_conn)
                st.on_conn(true);
            if (gen != st.gen)
                return; // the callback stopped or closed us
            self->readLoop(gen);
        });
    });
}

void Dialer::lost(uint64_t gen, const std::string& why)
{
    auto& m = *impl_;
    if (gen != m.gen)
        return;
    ++m.gen; // invalidates every handler of this connection
    const bool was_up = m.up.exchange(false);
    boost::system::error_code ec;
    m.resolver.cancel();
    if (m.socket)
        m.socket->close(ec);
    m.socket.reset();
    m.queue.clear();
    m.in_flight = 0;
    m.queued_bytes = 0;
    m.counters.last_error = why;
    log(m.failed_before && !was_up ? LogLevel::debug : LogLevel::warn, why);
    m.failed_before = true;
    if (m.running) {
        const auto delay = m.backoff.next();
        std::weak_ptr<Dialer> weak = shared_from_this();
        m.retry.after(delay, [weak] {
            if (auto self = weak.lock(); self && self->impl_->running)
                self->connect();
        });
    }
    if (was_up && m.on_conn) {
        // Always asynchronous: the caller of send() must not be re-entered.
        std::weak_ptr<Dialer> weak = shared_from_this();
        const uint64_t epoch = m.stop_epoch;
        asio::post(m.io, [weak, epoch] {
            auto self = weak.lock();
            if (self && self->impl_->stop_epoch == epoch && self->impl_->on_conn)
                self->impl_->on_conn(false);
        });
    }
}

void Dialer::readLoop(uint64_t gen)
{
    auto& im = *impl_;
    im.rbuf.resize(im.opts.chunk_bytes);
    auto self = shared_from_this();
    auto sock = im.socket;
    sock->async_read_some(asio::buffer(im.rbuf),
                              [self, gen, sock](const boost::system::error_code& ec, std::size_t n) {
        auto& m = *self->impl_;
        if (gen != m.gen)
            return;
        if (ec) {
            self->lost(gen, std::string("read: ") + ec.message());
            return;
        }
        m.counters.bytes_in += n;
        if (m.on_data)
            m.on_data(m.rbuf.data(), n);
        if (gen != m.gen)
            return;
        self->readLoop(gen);
    });
}

void Dialer::send(Buf b)
{
    auto& m = *impl_;
    if (!b)
        return;
    if (!m.up) {
        ++m.counters.dropped;
        return;
    }
    if (m.queued_bytes + b->size() > m.opts.send_cap) {
        ++m.counters.dropped;
        lost(m.gen, "send queue over " + std::to_string(m.opts.send_cap) + " bytes");
        return;
    }
    m.queued_bytes += b->size();
    m.queue.push_back(std::move(b));
    if (m.in_flight == 0)
        writeLoop(m.gen);
}

void Dialer::writeLoop(uint64_t gen)
{
    auto& im = *impl_;
    if (im.queue.empty())
        return;
    im.in_flight = im.queue.size();
    std::vector<asio::const_buffer> bufs;
    bufs.reserve(im.in_flight);
    for (std::size_t i = 0; i < im.in_flight; ++i)
        bufs.emplace_back(im.queue[i]->data(), im.queue[i]->size());
    auto self = shared_from_this();
    auto sock = im.socket;
    asio::async_write(*sock, bufs, [self, gen, sock](const boost::system::error_code& ec, std::size_t n) {
        auto& m = *self->impl_;
        if (gen != m.gen)
            return;
        if (ec) {
            self->lost(gen, std::string("write: ") + ec.message());
            return;
        }
        m.counters.bytes_out += n;
        for (std::size_t i = 0; i < m.in_flight; ++i) {
            m.queued_bytes -= m.queue.front()->size();
            m.queue.pop_front();
            ++m.counters.messages_out;
        }
        m.in_flight = 0;
        self->writeLoop(gen);
    });
}

} // namespace skycue::net
