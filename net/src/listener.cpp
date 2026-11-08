#include "skycue/net/listener.hpp"

#include "skycue/net/backoff.hpp"
#include "skycue/net/endpoint.hpp"
#include "skycue/net/log.hpp"
#include "skycue/net/write_queue.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <map>
#include <stdexcept>
#include <vector>

namespace skycue::net {

namespace asio = boost::asio;
using asio::ip::tcp;

namespace {
constexpr std::chrono::milliseconds kAcceptRetry{100};
constexpr std::chrono::seconds kAcceptWarnEvery{10};
} // namespace

struct Listener::Session {
    Session(asio::io_context& io, uint64_t id, std::size_t cap)
        : stream(id), socket(std::make_shared<tcp::socket>(io)), wq(socket, cap)
    {
    }
    uint64_t stream;
    std::shared_ptr<tcp::socket> socket; // handlers of composed operations hold the session, so a close is safe
    tcp::endpoint who;
    std::vector<uint8_t> rbuf;
    bool closed = false;
    WriteQueue wq;
};

struct Listener::Impl {
    Impl(asio::io_context& io_, std::string b, uint16_t p, ListenOptions o_)
        : io(io_), bind(std::move(b)), port(p), opts(std::move(o_)), rebind(io_), accept_retry(io_)
    {
    }
    asio::io_context& io;
    std::string bind;
    uint16_t port;
    ListenOptions opts;
    bool has_peer_filter = false;
    asio::ip::address peer_addr;
    RetryTimer rebind;
    RetryTimer accept_retry;
    std::shared_ptr<tcp::acceptor> acceptor;
    std::map<uint64_t, std::shared_ptr<Session>> sessions;
    uint64_t next_stream = 1;
    uint64_t gen = 0;        // changes on start() and stop(); voids handlers of an earlier run
    uint64_t stop_epoch = 0; // changes on stop(); suppresses notifications posted earlier
    bool running = false;
    bool bind_failed_before = false;
    bool accept_warned = false;
    std::chrono::steady_clock::time_point last_accept_warn;
    std::atomic<bool> up{false};
    std::atomic<uint16_t> bound_port{0};
    std::function<void(uint64_t, const uint8_t*, std::size_t)> on_data;
    std::function<void(uint64_t, bool, const tcp::endpoint&)> on_peer;
    mutable Counters counters;
    uint64_t closed_bytes_out = 0;    // written by sessions that are gone
    uint64_t closed_messages_out = 0;
};

std::shared_ptr<Listener> Listener::create(asio::io_context& io, std::string bind, uint16_t port, ListenOptions o)
{
    return std::shared_ptr<Listener>(new Listener(io, std::move(bind), port, std::move(o)));
}

Listener::Listener(asio::io_context& io, std::string bind, uint16_t port, ListenOptions o)
    : impl_(std::make_unique<Impl>(io, std::move(bind), port, std::move(o)))
{
    auto& m = *impl_;
    if (!m.opts.peer.empty()) {
        boost::system::error_code ec;
        m.peer_addr = unmapV4(asio::ip::make_address(m.opts.peer, ec));
        if (ec)
            throw std::invalid_argument("listener peer is not an IP address: " + m.opts.peer);
        m.has_peer_filter = true;
    }
}

Listener::~Listener() = default;

void Listener::onData(std::function<void(uint64_t, const uint8_t*, std::size_t)> h) { impl_->on_data = std::move(h); }
void Listener::onPeer(std::function<void(uint64_t, bool, const tcp::endpoint&)> h) { impl_->on_peer = std::move(h); }
bool Listener::listening() const { return impl_->up.load(); }
uint16_t Listener::localPort() const { return impl_->bound_port.load(); }
std::size_t Listener::peers() const { return impl_->sessions.size(); }
std::optional<PeerStats> Listener::peerStats(uint64_t stream) const
{
    const auto it = impl_->sessions.find(stream);
    if (it == impl_->sessions.end())
        return std::nullopt;
    const WriteQueue& q = it->second->wq;
    return PeerStats{q.bytesWritten(), q.messagesWritten(), q.queuedBytes()};
}
const Counters& Listener::counters() const
{
    auto& m = *impl_;
    m.counters.bytes_out = m.closed_bytes_out;
    m.counters.messages_out = m.closed_messages_out;
    for (const auto& kv : m.sessions) {
        m.counters.bytes_out += kv.second->wq.bytesWritten();
        m.counters.messages_out += kv.second->wq.messagesWritten();
    }
    return m.counters;
}

void Listener::start()
{
    auto& m = *impl_;
    if (m.running)
        return;
    m.running = true;
    ++m.gen;
    m.bind_failed_before = false;
    bindAndListen(m.opts.fail_fast);
}

void Listener::stop()
{
    auto& m = *impl_;
    m.running = false;
    ++m.gen;
    ++m.stop_epoch;
    m.rebind.cancel();
    m.accept_retry.cancel();
    boost::system::error_code ec;
    if (m.acceptor)
        m.acceptor->close(ec);
    m.acceptor.reset();
    for (auto& kv : m.sessions) {
        kv.second->closed = true;
        kv.second->socket->close(ec);
        retire(*kv.second);
    }
    m.sessions.clear();
    m.counters.peers = 0;
    m.up = false;
    m.bound_port = 0;
}

void Listener::bindAndListen(bool throwOnFailure)
{
    auto& m = *impl_;
    std::string why;
    auto acc = std::make_shared<tcp::acceptor>(m.io);
    do {
        boost::system::error_code ec;
        const auto addr = asio::ip::make_address(m.bind.empty() ? "0.0.0.0" : m.bind, ec);
        if (ec) {
            why = "bind address " + m.bind + ": " + ec.message();
            break;
        }
        const tcp::endpoint ep(addr, m.port);
        acc->open(ep.protocol(), ec);
        if (!ec)
            acc->set_option(tcp::acceptor::reuse_address(true), ec);
        if (!ec)
            acc->bind(ep, ec);
        if (!ec)
            acc->listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            why = "listen on " + m.bind + ":" + std::to_string(m.port) + ": " + ec.message();
            break;
        }
    } while (false);

    if (!why.empty()) {
        boost::system::error_code ignore;
        acc->close(ignore);
        m.counters.last_error = why;
        log(m.bind_failed_before ? LogLevel::debug : LogLevel::warn, why);
        m.bind_failed_before = true;
        if (throwOnFailure) {
            m.running = false;
            ++m.gen;
            throw std::runtime_error(why);
        }
        std::weak_ptr<Listener> weak = shared_from_this();
        const uint64_t gen = m.gen;
        m.rebind.after(m.opts.rebind_delay, [weak, gen] {
            auto self = weak.lock();
            if (self && self->impl_->running && self->impl_->gen == gen)
                self->bindAndListen(false);
        });
        return;
    }

    boost::system::error_code ec;
    m.acceptor = acc;
    m.bound_port = acc->local_endpoint(ec).port();
    m.bind_failed_before = false;
    m.counters.last_error.clear();
    m.up = true;
    log(LogLevel::info, "listening on " + m.bind + ":" + std::to_string(m.bound_port.load()));
    acceptLoop(m.gen, acc);
}

void Listener::acceptLoop(uint64_t gen, const std::shared_ptr<tcp::acceptor>& acc)
{
    auto& im = *impl_;
    auto s = std::make_shared<Session>(im.io, 0, im.opts.tcp.send_cap);
    auto self = shared_from_this();
    acc->async_accept(*s->socket, s->who, [self, gen, acc, s](const boost::system::error_code& ec) {
        auto& m = *self->impl_;
        if (gen != m.gen)
            return;
        if (ec) {
            if (ec == asio::error::operation_aborted)
                return;
            m.counters.last_error = "accept: " + ec.message();
            const auto now = std::chrono::steady_clock::now();
            if (!m.accept_warned || now - m.last_accept_warn >= kAcceptWarnEvery) {
                m.accept_warned = true;
                m.last_accept_warn = now;
                log(LogLevel::warn, m.counters.last_error);
            } else {
                log(LogLevel::debug, m.counters.last_error);
            }
            std::weak_ptr<Listener> weak = self;
            m.accept_retry.after(kAcceptRetry, [weak, gen, acc] {
                auto l = weak.lock();
                if (l && l->impl_->gen == gen)
                    l->acceptLoop(gen, acc);
            });
            return;
        }
        self->adopt(s);
        if (gen != m.gen)
            return; // a callback stopped us
        self->acceptLoop(gen, acc);
    });
}

void Listener::adopt(const std::shared_ptr<Session>& s)
{
    auto& m = *impl_;
    boost::system::error_code ec;
    const bool filtered = m.has_peer_filter && unmapV4(s->who.address()) != m.peer_addr;
    if (filtered || m.sessions.size() >= m.opts.max_peers) {
        ++m.counters.rejected_peer;
        log(LogLevel::debug, std::string("rejected peer ") + s->who.address().to_string() +
                                 (filtered ? " (not the configured peer)" : " (peer limit)"));
        s->closed = true;
        s->socket->close(ec);
        return;
    }
    s->stream = m.next_stream++;
    applyTcpOptions(*s->socket, m.opts.tcp);
    m.sessions[s->stream] = s;
    {
        std::weak_ptr<Listener> weak = shared_from_this();
        std::weak_ptr<Session> ws = s;
        s->wq.start([weak, ws](const boost::system::error_code& wec) {
            auto self = weak.lock();
            auto sess = ws.lock();
            if (self && sess && !sess->closed)
                self->closeSession(sess, std::string("write: ") + wec.message());
        });
    }
    m.counters.peers = m.sessions.size();
    ++m.counters.connects;
    log(LogLevel::info, "peer " + s->who.address().to_string() + ":" + std::to_string(s->who.port()) +
                            " connected as stream " + std::to_string(s->stream));
    const uint64_t gen = m.gen;
    if (const auto hook = m.on_peer) // a copy: the hook may replace the handler
        hook(s->stream, true, s->who);
    if (gen != m.gen || s->closed)
        return; // the callback stopped us or closed this peer
    readLoop(gen, s);
}

void Listener::closeSession(const std::shared_ptr<Session>& s, const std::string& why)
{
    auto& m = *impl_;
    if (s->closed)
        return;
    s->closed = true;
    boost::system::error_code ec;
    s->socket->close(ec);
    retire(*s);
    m.sessions.erase(s->stream);
    m.counters.peers = m.sessions.size();
    log(LogLevel::debug, "peer " + s->who.address().to_string() + " (stream " + std::to_string(s->stream) +
                             ") closed: " + why);
    if (m.on_peer) {
        // Always asynchronous: the caller of send() must not be re-entered.
        std::weak_ptr<Listener> weak = shared_from_this();
        const uint64_t epoch = m.stop_epoch;
        const uint64_t stream = s->stream;
        const tcp::endpoint who = s->who;
        asio::post(m.io, [weak, epoch, stream, who] {
            auto self = weak.lock();
            if (!self || self->impl_->stop_epoch != epoch)
                return;
            if (const auto hook = self->impl_->on_peer) // a copy: the hook may replace the handler
                hook(stream, false, who);
        });
    }
}

void Listener::readLoop(uint64_t gen, const std::shared_ptr<Session>& s)
{
    auto& im = *impl_;
    s->rbuf.resize(im.opts.tcp.chunk_bytes);
    auto self = shared_from_this();
    s->socket->async_read_some(asio::buffer(s->rbuf),
                               [self, gen, s](const boost::system::error_code& ec, std::size_t n) {
        auto& m = *self->impl_;
        if (gen != m.gen || s->closed)
            return;
        if (ec) {
            self->closeSession(s, std::string("read: ") + ec.message());
            return;
        }
        m.counters.bytes_in += n;
        if (m.on_data)
            m.on_data(s->stream, s->rbuf.data(), n);
        if (gen != m.gen || s->closed)
            return;
        self->readLoop(gen, s);
    });
}

void Listener::send(uint64_t stream, const Buf& b)
{
    auto& m = *impl_;
    if (!b)
        return;
    const auto it = m.sessions.find(stream);
    if (it == m.sessions.end())
        return;
    const auto s = it->second; // sendTo may erase the entry
    sendTo(s, b);
}

void Listener::sendAll(const Buf& b)
{
    auto& m = *impl_;
    if (!b)
        return;
    std::vector<std::shared_ptr<Session>> all;
    all.reserve(m.sessions.size());
    for (auto& kv : m.sessions)
        all.push_back(kv.second);
    for (auto& s : all)
        sendTo(s, b);
}

void Listener::sendTo(const std::shared_ptr<Session>& s, const Buf& b)
{
    auto& m = *impl_;
    if (s->closed)
        return;
    if (b->empty()) {
        ++m.counters.dropped;
        return;
    }
    if (!s->wq.enqueue(b)) {
        ++m.counters.dropped;
        closeSession(s, "send queue over " + std::to_string(m.opts.tcp.send_cap) + " bytes");
    }
}

void Listener::retire(Session& s)
{
    auto& m = *impl_;
    m.closed_bytes_out += s.wq.bytesWritten();
    m.closed_messages_out += s.wq.messagesWritten();
    s.wq.clear();
}

} // namespace skycue::net
