#include "skycue/net/dialer.hpp"

#include "skycue/net/backoff.hpp"
#include "skycue/net/log.hpp"
#include "skycue/net/write_queue.hpp"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <vector>

namespace skycue::net {

namespace asio = boost::asio;
using asio::ip::tcp;

struct Dialer::Impl {
    Impl(asio::io_context& io_, std::string h, uint16_t p, TcpOptions o, std::vector<tcp::endpoint> f)
        : io(io_), host(std::move(h)), port(p), opts(std::move(o)), fixed(std::move(f)), resolver(io_), retry(io_),
          connect_timer(io_), backoff(opts.min_backoff, opts.max_backoff)
    {
    }
    // Folds the write counts of the connection that ended into the totals.
    void retireQueue()
    {
        if (!wq)
            return;
        closed_bytes_out += wq->bytesWritten();
        closed_messages_out += wq->messagesWritten();
        wq->clear();
        wq.reset();
    }
    asio::io_context& io;
    std::string host;
    uint16_t port;
    TcpOptions opts;
    std::vector<tcp::endpoint> fixed; // test-only: connect to these instead of resolving
    tcp::resolver resolver;
    RetryTimer retry;
    asio::steady_timer connect_timer; // the connect timeout of the address being tried
    Backoff backoff;
    std::shared_ptr<tcp::socket> socket; // handlers of composed operations hold it too, so a reset is safe
    uint64_t stop_epoch = 0; // changes on stop(); suppresses notifications posted earlier
    std::vector<uint8_t> rbuf;
    std::function<void(const uint8_t*, std::size_t)> on_data;
    std::function<void(bool)> on_conn;
    std::function<void(const std::string&)> on_fail;
    mutable Counters counters;
    uint64_t closed_bytes_out = 0;    // written on connections that ended
    uint64_t closed_messages_out = 0;
    uint64_t attempts = 0;
    uint64_t gen = 0;       // changes on every connection attempt and on stop()
    bool running = false;
    bool failed_before = false; // a failure since the last success was already logged at warn
    std::atomic<bool> up{false};
    std::unique_ptr<WriteQueue> wq; // exists while connected
    tcp::endpoint remote;           // of the current or latest connection
    PeerDown last_down;             // of the notification being delivered
    PeerDown pending_down;          // of a loss whose notification is still posted
    bool down_pending = false;
    // Delivers the posted loss notification now, or when its post runs.
    void deliverDown()
    {
        if (!down_pending)
            return;
        down_pending = false;
        last_down = pending_down;
        if (on_conn)
            on_conn(false);
    }
};

std::shared_ptr<Dialer> Dialer::create(asio::io_context& io, std::string host, uint16_t port, TcpOptions o)
{
    return std::shared_ptr<Dialer>(new Dialer(io, std::move(host), port, std::move(o), {}));
}

std::shared_ptr<Dialer> Dialer::create(asio::io_context& io, std::vector<tcp::endpoint> endpoints, TcpOptions o)
{
    std::string name = endpoints.empty() ? std::string("(none)") : endpoints.front().address().to_string();
    const uint16_t port = endpoints.empty() ? 0 : endpoints.front().port();
    return std::shared_ptr<Dialer>(new Dialer(io, std::move(name), port, std::move(o), std::move(endpoints)));
}

Dialer::Dialer(asio::io_context& io, std::string host, uint16_t port, TcpOptions o, std::vector<tcp::endpoint> fixed)
    : impl_(std::make_unique<Impl>(io, std::move(host), port, std::move(o), std::move(fixed)))
{
}

Dialer::~Dialer() = default;

void Dialer::onData(std::function<void(const uint8_t*, std::size_t)> h) { impl_->on_data = std::move(h); }
void Dialer::onConnection(std::function<void(bool)> h) { impl_->on_conn = std::move(h); }
void Dialer::onFailure(std::function<void(const std::string&)> h) { impl_->on_fail = std::move(h); }
const PeerDown& Dialer::downInfo() const { return impl_->last_down; }
bool Dialer::connected() const { return impl_->up.load(); }
const Counters& Dialer::counters() const
{
    auto& m = *impl_;
    m.counters.bytes_out = m.closed_bytes_out + (m.wq ? m.wq->bytesWritten() : 0);
    m.counters.messages_out = m.closed_messages_out + (m.wq ? m.wq->messagesWritten() : 0);
    return m.counters;
}
PeerStats Dialer::peerStats() const
{
    const auto& wq = impl_->wq;
    return wq ? PeerStats{wq->bytesWritten(), wq->messagesWritten(), wq->queuedBytes()} : PeerStats{};
}
tcp::endpoint Dialer::peerEndpoint() const { return impl_->remote; }
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
    m.down_pending = false;
    m.retry.cancel();
    m.connect_timer.cancel();
    boost::system::error_code ec;
    m.resolver.cancel();
    if (m.socket)
        m.socket->close(ec);
    m.socket.reset();
    m.up = false;
    m.retireQueue();
}

void Dialer::stopWithNotice()
{
    auto& m = *impl_;
    const bool was_up = m.up.load();
    stop();
    if (!was_up || !m.on_conn)
        return;
    m.pending_down = PeerDown{DownReason::shutdown, "closed by the host"};
    m.down_pending = true;
    std::weak_ptr<Dialer> weak = shared_from_this();
    const uint64_t epoch = m.stop_epoch;
    asio::post(m.io, [weak, epoch] {
        auto self = weak.lock();
        if (self && self->impl_->stop_epoch == epoch)
            self->impl_->deliverDown();
    });
}

// The addresses of one attempt and where it is in them.
struct Dialer::Attempt {
    std::vector<tcp::endpoint> eps;
    std::size_t next = 0;
    uint64_t step = 0;    // changes per address; voids the timer of an earlier one
    bool timed_out = false;
    std::string last_error;
};

void Dialer::connect()
{
    auto& im = *impl_;
    const uint64_t gen = ++im.gen;
    ++im.attempts;
    im.socket = std::make_shared<tcp::socket>(im.io);
    auto sock = im.socket;
    auto self = shared_from_this();
    if (!im.fixed.empty()) {
        auto at = std::make_shared<Attempt>();
        at->eps = im.fixed;
        connectNext(gen, sock, at);
        return;
    }
    im.resolver.async_resolve(im.host, std::to_string(im.port),
                             [self, gen, sock](const boost::system::error_code& ec, const tcp::resolver::results_type& results) {
        auto& m = *self->impl_;
        if (gen != m.gen)
            return;
        if (ec) {
            self->lost(gen, "resolve " + m.host + ": " + ec.message());
            return;
        }
        auto at = std::make_shared<Attempt>();
        for (const auto& r : results)
            at->eps.push_back(r.endpoint());
        self->connectNext(gen, sock, at);
    });
}

void Dialer::connectNext(uint64_t gen, const std::shared_ptr<tcp::socket>& sock, const std::shared_ptr<Attempt>& at)
{
    auto& m = *impl_;
    if (gen != m.gen)
        return;
    if (at->next >= at->eps.size()) {
        lost(gen, "connect " + m.host + ":" + std::to_string(m.port) + ": " +
                      (at->last_error.empty() ? "no address" : at->last_error));
        return;
    }
    const tcp::endpoint ep = at->eps[at->next++];
    const uint64_t step = ++at->step;
    at->timed_out = false;
    boost::system::error_code ec;
    sock->close(ec);
    sock->open(ep.protocol(), ec);
    if (ec) {
        at->last_error = ep.address().to_string() + ": " + ec.message();
        connectNext(gen, sock, at);
        return;
    }
    auto self = shared_from_this();
    if (m.opts.connect_timeout.count() > 0) {
        m.connect_timer.expires_after(m.opts.connect_timeout);
        m.connect_timer.async_wait([self, gen, sock, at, step](const boost::system::error_code& tec) {
            auto& st = *self->impl_;
            if (tec || gen != st.gen || at->step != step)
                return;
            at->timed_out = true;
            boost::system::error_code ignore;
            sock->close(ignore); // aborts the connect, whose handler moves on
        });
    }
    sock->async_connect(ep, [self, gen, sock, at, ep, step](const boost::system::error_code& cec) {
        auto& st = *self->impl_;
        if (gen != st.gen)
            return;
        // Void the timer of this address, also when its handler is already queued.
        const bool timed_out = at->timed_out;
        ++at->step;
        st.connect_timer.cancel();
        if (cec) {
            at->last_error = ep.address().to_string() + ": " + (timed_out ? std::string("timed out") : cec.message());
            self->connectNext(gen, sock, at);
            return;
        }
        self->connected(gen, sock);
    });
}

void Dialer::connected(uint64_t gen, const std::shared_ptr<tcp::socket>& sock)
{
    auto& st = *impl_;
    applyTcpOptions(*sock, st.opts);
    st.backoff.reset();
    st.failed_before = false;
    ++st.counters.connects;
    st.counters.last_error.clear();
    // A loss whose notification is still posted comes first, so the host
    // sees down for the old connection before up for the new one. This runs
    // before the new connection becomes visible (up, the write queue, the
    // peer): a handler for the old connection that stops or closes the dialer
    // sees the old state, and connected()/peerInfo never report the new
    // connection while the old one's down is delivered. The inline path is
    // unreachable through posting order today; the order is defensive.
    st.deliverDown();
    if (gen != st.gen)
        return;
    st.wq = std::make_unique<WriteQueue>(sock, st.opts.send_cap);
    {
        std::weak_ptr<Dialer> weak = shared_from_this();
        st.wq->start([weak, gen](const boost::system::error_code& wec) {
            if (auto self = weak.lock())
                self->lost(gen, std::string("write: ") + wec.message());
        });
    }
    {
        boost::system::error_code rec;
        const auto ep = sock->remote_endpoint(rec);
        if (!rec)
            st.remote = ep;
    }
    st.up = true;
    log(LogLevel::debug, st.opts.name, "connected to " + st.host + ":" + std::to_string(st.port));
    if (st.on_conn)
        st.on_conn(true);
    if (gen != st.gen)
        return; // the callback stopped or closed us
    readLoop(gen);
}

void Dialer::lost(uint64_t gen, const std::string& why, DownReason reason)
{
    auto& m = *impl_;
    if (gen != m.gen)
        return;
    ++m.gen; // invalidates every handler of this connection
    const bool was_up = m.up.exchange(false);
    boost::system::error_code ec;
    m.resolver.cancel();
    m.connect_timer.cancel();
    if (m.socket)
        m.socket->close(ec);
    m.socket.reset();
    m.retireQueue();
    m.counters.last_error = why;
    // The first failed connect attempt since the last success is a warning, the
    // following ones debug; a warning names the peer (the debug line is read
    // next to it). The loss of an established connection is debug: the host
    // logs it in its own words, and the next failed attempt is the first.
    const bool quiet = was_up || m.failed_before;
    m.failed_before = !was_up;
    if (m.running) {
        const auto delay = m.backoff.next();
        const std::string retrying = "; retrying in " + std::to_string(delay.count()) + " ms";
        if (quiet)
            log(LogLevel::debug, m.opts.name, why + retrying);
        else
            log(LogLevel::warn, m.opts.name,
                (why.rfind("resolve ", 0) == 0 || why.rfind("connect ", 0) == 0
                     ? std::string()
                     : m.host + ":" + std::to_string(m.port) + ": ") +
                    why + retrying +
                    " (further failures logged at debug level)");
        std::weak_ptr<Dialer> weak = shared_from_this();
        m.retry.after(delay, [weak] {
            if (auto self = weak.lock(); self && self->impl_->running)
                self->connect();
        });
    } else {
        log(quiet ? LogLevel::debug : LogLevel::warn, m.opts.name, why);
    }
    if (!was_up && m.on_fail) {
        // A failed attempt cycle; asynchronous like the connection events.
        std::weak_ptr<Dialer> weak = shared_from_this();
        const uint64_t epoch = m.stop_epoch;
        asio::post(m.io, [weak, epoch, why] {
            auto self = weak.lock();
            if (self && self->impl_->stop_epoch == epoch && self->impl_->on_fail)
                self->impl_->on_fail(why);
        });
    }
    if (was_up && m.on_conn) {
        // Always asynchronous: the caller of send() must not be re-entered.
        std::weak_ptr<Dialer> weak = shared_from_this();
        const uint64_t epoch = m.stop_epoch;
        m.pending_down = PeerDown{reason, why};
        m.down_pending = true;
        asio::post(m.io, [weak, epoch] {
            auto self = weak.lock();
            if (self && self->impl_->stop_epoch == epoch)
                self->impl_->deliverDown();
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
            self->lost(gen, std::string("read: ") + ec.message(),
                       ec == asio::error::eof ? DownReason::closed : DownReason::error);
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

void Dialer::send(const Buf& b)
{
    auto& m = *impl_;
    if (!b)
        return;
    if (!m.up || b->empty()) {
        ++m.counters.dropped;
        return;
    }
    if (!m.wq->enqueue(b)) {
        ++m.counters.dropped;
        lost(m.gen, "send queue over " + std::to_string(m.opts.send_cap) + " bytes", DownReason::over_cap);
    }
}

} // namespace skycue::net
