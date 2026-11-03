// Forwarder. Single-threaded: everything runs on the caller's io_context
// thread, so there are no locks. Pending handlers hold a shared_ptr to Impl
// (and to their Session), so none of them ever touches a destroyed object;
// `closed` flags and generation counters make stale handlers do nothing.
// Asio API kept to what Boost 1.66 offers.

#include "forwarder.hpp"

#include "log.hpp"

#include <skycue/protocol.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <optional>
#include <stdexcept>

namespace skycued {

namespace asio = boost::asio;
using asio::ip::tcp;
using asio::ip::udp;
using error_code = boost::system::error_code;
using steady = std::chrono::steady_clock;
using Buf = std::shared_ptr<const std::vector<uint8_t>>;

namespace {
constexpr std::chrono::milliseconds kMinInterval{10};
constexpr std::chrono::seconds kUdpErrorLogEvery{60};
constexpr std::chrono::seconds kStopDrainLimit{2};
constexpr std::chrono::milliseconds kAcceptRetry{100};
// A persistent accept failure (EMFILE, say) retries every kAcceptRetry; it
// is logged at most this often, with a count of the failures in between.
constexpr std::chrono::seconds kAcceptWarnEvery{10};
constexpr std::chrono::milliseconds kDialBackoffMin{500};
constexpr std::chrono::milliseconds kDialBackoffMax{10000};

std::chrono::milliseconds clampInterval(std::chrono::milliseconds ms) { return std::max(ms, kMinInterval); }

uint32_t toMs32(std::chrono::milliseconds ms)
{
    const auto c = ms.count();
    return c > 0xFFFFFFFFLL ? 0xFFFFFFFFu : static_cast<uint32_t>(c);
}

Buf share(std::vector<uint8_t> v) { return std::make_shared<const std::vector<uint8_t>>(std::move(v)); }
} // namespace

std::string endpointText(const std::string& address, uint16_t port)
{
    const bool v6 = address.find(':') != std::string::npos;
    return (v6 ? "[" + address + "]" : address) + ":" + std::to_string(port);
}

struct Forwarder::Impl : std::enable_shared_from_this<Forwarder::Impl> {
    Impl(asio::io_context& io_, ForwarderConfig c)
        : io(io_), cfg(std::move(c)), keyframe_interval(clampInterval(cfg.keyframe_interval)),
          heartbeat_interval(clampInterval(cfg.heartbeat_interval))
    {
    }

    struct OutboundLink;

    struct Session {
        explicit Session(asio::io_context& io, uint64_t id_) : sock(io), id(id_) {}
        tcp::socket sock;
        uint64_t id;
        std::string peer;
        std::string kind = "TCP client";     // for the log: "TCP client" or "TCP target"
        std::shared_ptr<OutboundLink> link; // set for a connection we dialled
        std::chrono::system_clock::time_point connected_since{}; // set by addSession
        uint64_t bytes_sent = 0;        // bytes whose async_write completed
        std::deque<Buf> queue;         // not yet handed to async_write
        std::vector<Buf> inflight;      // being written now
        size_t queued_bytes = 0;        // queue + inflight
        bool closed = false;
        bool close_when_drained = false; // stop(): close once everything is written
        std::array<uint8_t, 512> rx{};  // clients send nothing; read only to see EOF
    };

    // One of cfg.tcp_connect_targets: dials, and dials again whenever the
    // connection fails or closes. `gen` turns off a cancelled attempt.
    struct OutboundLink {
        OutboundLink(asio::io_context& io, std::string h, uint16_t p)
            : host(std::move(h)), port(p), name(host + ":" + std::to_string(port)), resolver(io), timer(io)
        {
        }
        std::string host;
        uint16_t port;
        std::string name;
        tcp::resolver resolver;
        asio::steady_timer timer;
        std::shared_ptr<Session> dialling; // socket being connected
        std::chrono::milliseconds backoff = kDialBackoffMin;
        unsigned gen = 0;
        bool failure_logged = false; // warn once per outage, not per attempt
    };

    struct UdpTarget {
        std::string name;
        udp::endpoint ep;
        bool error_seen = false;
        steady::time_point last_error_log{};
        uint64_t errors_since_log = 0;
        uint64_t datagrams_sent = 0; // sends completed without error
    };

    asio::io_context& io;
    ForwarderConfig cfg;
    std::chrono::milliseconds keyframe_interval, heartbeat_interval;
    KeyframeProvider provider;

    tcp::acceptor acceptor{io};
    udp::socket udp_sock{io};
    asio::steady_timer keyframe_timer{io};
    asio::steady_timer heartbeat_timer{io};
    asio::steady_timer accept_retry_timer{io};
    asio::steady_timer stop_timer{io};

    std::map<uint64_t, std::shared_ptr<Session>> sessions;
    std::vector<UdpTarget> targets;
    std::vector<std::shared_ptr<OutboundLink>> links;
    uint64_t next_session_id = 1;
    size_t udp_inflight = 0;

    bool started = false, stopping = false, dead = false;
    unsigned keyframe_gen = 0, heartbeat_gen = 0, accept_gen = 0;
    std::optional<steady::time_point> last_accept_warn;
    unsigned accept_failures_unlogged = 0;
    steady::time_point next_keyframe{}, last_broadcast{};
    uint64_t frames_sent = 0;
    uint16_t port = 0;

    // ---- start -----------------------------------------------------------

    void start()
    {
        if (started || stopping)
            return;
        const bool listening = cfg.tcp_listen_ephemeral || cfg.tcp_port != 0;
        if (listening)
            listen();
        started = true;
        if (!listening)
            LOG_INFO("forwarder: TCP listener disabled (CUE_TCP_PORT=0)");

        resolveTargets();
        helloToUdp();

        const auto now = steady::now();
        last_broadcast = now;
        next_keyframe = now + keyframe_interval;
        armKeyframe();
        armHeartbeat();
        if (listening)
            accept();
        for (const auto& t : cfg.tcp_connect_targets) {
            links.push_back(std::make_shared<OutboundLink>(io, t.first, t.second));
            LOG_INFO("forwarder: TCP target " << links.back()->name);
            dial(links.back());
        }
    }

    void listen()
    {
        error_code ec;
        const tcp::endpoint ep(tcp::v4(), cfg.tcp_listen_ephemeral ? uint16_t{0} : cfg.tcp_port);
        acceptor.open(ep.protocol(), ec);
        if (!ec)
            acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
        if (!ec)
            acceptor.bind(ep, ec);
        if (!ec)
            acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            error_code ignored;
            acceptor.close(ignored);
            throw std::runtime_error("forwarder: cannot listen on TCP port " + std::to_string(cfg.tcp_port) + ": " +
                                     ec.message());
        }
        port = acceptor.local_endpoint(ec).port();
        LOG_INFO("forwarder: listening on TCP port " << port);
    }

    void resolveTargets()
    {
        for (const auto& t : cfg.udp_targets) {
            const std::string name = t.first + ":" + std::to_string(t.second);
            error_code ec;
            udp::endpoint ep;
            const auto addr = asio::ip::make_address(t.first, ec);
            if (!ec) {
                ep = udp::endpoint(addr, t.second);
            } else {
                udp::resolver r(io);
                const auto results = r.resolve(udp::v4(), t.first, std::to_string(t.second), ec);
                if (!ec && !results.empty())
                    ep = *results.begin();
                else if (!ec)
                    ec = asio::error::host_not_found;
            }
            if (!ec && !ep.address().is_v4())
                ec = asio::error::address_family_not_supported;
            if (ec) {
                LOG_ERROR("forwarder: UDP target " << name << " skipped: " << ec.message());
                continue;
            }
            targets.push_back(UdpTarget{name, ep});
            LOG_INFO("forwarder: UDP target " << name);
        }
        if (targets.empty())
            return;
        error_code ec;
        udp_sock.open(udp::v4(), ec);
        if (ec) {
            LOG_ERROR("forwarder: cannot open UDP socket: " << ec.message());
            targets.clear();
        }
    }

    // ---- TCP accept / sessions -------------------------------------------

    void accept()
    {
        if (stopping)
            return;
        auto self = shared_from_this();
        auto s = std::make_shared<Session>(io, next_session_id++);
        const unsigned gen = accept_gen;
        acceptor.async_accept(s->sock, [self, s, gen](const error_code& ec) { self->onAccept(s, gen, ec); });
    }

    void onAccept(const std::shared_ptr<Session>& s, unsigned gen, const error_code& ec)
    {
        if (stopping || gen != accept_gen)
            return;
        if (ec) {
            if (ec == asio::error::operation_aborted)
                return;
            const auto now = steady::now();
            if (!last_accept_warn || now - *last_accept_warn >= kAcceptWarnEvery) {
                if (accept_failures_unlogged > 0)
                    LOG_WARN("forwarder: accept failed: " << ec.message() << " (and " << accept_failures_unlogged
                                                          << " more times since the last report)");
                else
                    LOG_WARN("forwarder: accept failed: " << ec.message());
                last_accept_warn = now;
                accept_failures_unlogged = 0;
            } else {
                ++accept_failures_unlogged;
            }
            auto self = shared_from_this();
            accept_retry_timer.expires_after(kAcceptRetry);
            accept_retry_timer.async_wait([self, gen](const error_code& tec) {
                if (!tec && !self->stopping && gen == self->accept_gen)
                    self->accept();
            });
            return;
        }
        error_code pec;
        const auto remote = s->sock.remote_endpoint(pec);
        s->peer = pec ? std::string("?") : endpointText(remote.address().to_string(), remote.port());
        addSession(s);
        accept();
    }

    // A connected socket, accepted or dialled: greet it and add it to the
    // broadcast set.
    void addSession(const std::shared_ptr<Session>& s)
    {
        error_code ignored;
        s->sock.set_option(tcp::no_delay(true), ignored);
        s->connected_since = std::chrono::system_clock::now();
        sessions.emplace(s->id, s);
        LOG_INFO("forwarder: " << s->kind << " " << s->peer << " connected (" << sessions.size() << " total)");

        read(s);
        enqueue(s, helloFrame());
        for (const Buf& b : keyframeFrames()) {
            if (s->closed)
                break;
            enqueue(s, b);
        }
    }

    // ---- TCP outbound ------------------------------------------------------

    void dial(const std::shared_ptr<OutboundLink>& l)
    {
        if (stopping)
            return;
        const unsigned gen = ++l->gen;
        auto self = shared_from_this();
        l->resolver.async_resolve(
            l->host, std::to_string(l->port),
            [self, l, gen](const error_code& ec, tcp::resolver::results_type results) {
                if (self->stopping || gen != l->gen)
                    return;
                if (ec) {
                    self->dialFailed(l, ec);
                    return;
                }
                auto s = std::make_shared<Session>(self->io, self->next_session_id++);
                s->kind = "TCP target";
                s->peer = l->name;
                s->link = l;
                l->dialling = s;
                asio::async_connect(s->sock, results, [self, l, s, gen](const error_code& cec, const tcp::endpoint&) {
                    if (l->dialling == s)
                        l->dialling.reset();
                    if (self->stopping || gen != l->gen)
                        return;
                    if (cec) {
                        error_code ignored;
                        s->sock.close(ignored);
                        self->dialFailed(l, cec);
                        return;
                    }
                    l->backoff = kDialBackoffMin;
                    l->failure_logged = false;
                    self->addSession(s);
                });
            });
    }

    void dialFailed(const std::shared_ptr<OutboundLink>& l, const error_code& ec)
    {
        if (!l->failure_logged) {
            LOG_WARN("forwarder: TCP target " << l->name << ": " << ec.message() << "; retrying");
            l->failure_logged = true;
        } else {
            LOG_DEBUG("forwarder: TCP target " << l->name << ": " << ec.message());
        }
        redial(l);
    }

    // Dials again after the current backoff, then doubles it.
    void redial(const std::shared_ptr<OutboundLink>& l)
    {
        if (stopping)
            return;
        const unsigned gen = ++l->gen;
        auto self = shared_from_this();
        l->timer.expires_after(l->backoff);
        l->timer.async_wait([self, l, gen](const error_code& ec) {
            if (!ec && !self->stopping && gen == l->gen)
                self->dial(l);
        });
        l->backoff = std::min(l->backoff * 2, kDialBackoffMax);
    }

    void cancelLinks()
    {
        error_code ignored;
        for (const auto& l : links) {
            ++l->gen;
            l->timer.cancel();
            l->resolver.cancel();
            if (l->dialling) {
                l->dialling->sock.close(ignored);
                l->dialling.reset();
            }
        }
    }

    void read(const std::shared_ptr<Session>& s)
    {
        auto self = shared_from_this();
        s->sock.async_read_some(asio::buffer(s->rx), [self, s](const error_code& ec, size_t) {
            if (s->closed)
                return;
            if (ec) {
                self->dropSession(s, ec == asio::error::eof ? std::string("disconnected") : ec.message(), false);
                return;
            }
            self->read(s); // anything a client sends is ignored
        });
    }

    void enqueue(const std::shared_ptr<Session>& s, const Buf& b)
    {
        if (s->closed || s->close_when_drained)
            return;
        s->queue.push_back(b);
        s->queued_bytes += b->size();
        ++frames_sent;
        if (s->queued_bytes > cfg.max_client_queue_bytes) {
            dropSession(s, "outbound queue exceeded " + std::to_string(cfg.max_client_queue_bytes) + " bytes", true);
            return;
        }
        if (s->inflight.empty())
            write(s);
    }

    void write(const std::shared_ptr<Session>& s)
    {
        if (s->closed)
            return;
        if (s->queue.empty()) {
            if (s->close_when_drained)
                dropSession(s, "closed for shutdown", false);
            return;
        }
        // Gather everything queued into one write.
        std::vector<asio::const_buffer> bufs;
        bufs.reserve(s->queue.size());
        for (auto& b : s->queue) {
            bufs.emplace_back(asio::buffer(*b));
            s->inflight.push_back(std::move(b));
        }
        s->queue.clear();
        auto self = shared_from_this();
        asio::async_write(s->sock, bufs, [self, s](const error_code& ec, size_t n) {
            if (s->closed)
                return;
            if (ec) {
                self->dropSession(s, "write failed: " + ec.message(), false);
                return;
            }
            s->bytes_sent += n;
            s->queued_bytes -= std::min(n, s->queued_bytes);
            s->inflight.clear();
            self->write(s);
        });
    }

    void dropSession(const std::shared_ptr<Session>& s, const std::string& why, bool warn)
    {
        if (s->closed)
            return;
        s->closed = true;
        error_code ignored;
        if (s->close_when_drained)
            s->sock.shutdown(tcp::socket::shutdown_send, ignored);
        s->sock.close(ignored);
        s->queue.clear();
        s->inflight.clear();
        s->queued_bytes = 0;
        sessions.erase(s->id);
        if (warn)
            LOG_WARN("forwarder: " << s->kind << " " << s->peer << " closed: " << why);
        else
            LOG_INFO("forwarder: " << s->kind << " " << s->peer << " " << why << " (" << sessions.size() << " left)");
        if (s->link && !stopping)
            redial(s->link);
        maybeFinishStop();
    }

    // ---- broadcast ---------------------------------------------------------

    void broadcast(const Buf& b)
    {
        last_broadcast = steady::now();
        std::vector<std::shared_ptr<Session>> all;
        all.reserve(sessions.size());
        for (const auto& kv : sessions)
            all.push_back(kv.second);
        for (const auto& s : all)
            enqueue(s, b);
        for (auto& t : targets)
            sendUdp(t, b);
    }

    void sendUdp(UdpTarget& t, const Buf& b)
    {
        if (!udp_sock.is_open())
            return;
        if (b->size() > skycue::kHeaderSize + skycue::kMaxUdpPayload) {
            LOG_ERROR("forwarder: frame of " << b->size() << " bytes too large for UDP, not sent");
            return;
        }
        ++frames_sent;
        ++udp_inflight;
        auto self = shared_from_this();
        UdpTarget* tp = &t; // targets is never resized after start()
        udp_sock.async_send_to(asio::buffer(*b), t.ep, [self, b, tp](const error_code& ec, size_t) {
            --self->udp_inflight;
            if (!ec)
                ++tp->datagrams_sent;
            else if (ec != asio::error::operation_aborted)
                self->udpError(*tp, ec);
            self->maybeFinishStop();
        });
    }

    void udpError(UdpTarget& t, const error_code& ec)
    {
        const auto now = steady::now();
        if (!t.error_seen) {
            t.error_seen = true;
            t.last_error_log = now;
            LOG_WARN("forwarder: UDP send to " << t.name << " failed: " << ec.message());
            return;
        }
        ++t.errors_since_log;
        if (now - t.last_error_log >= kUdpErrorLogEvery) {
            LOG_WARN("forwarder: UDP send to " << t.name << " failed: " << ec.message() << " ("
                                               << t.errors_since_log << " errors in the last minute)");
            t.last_error_log = now;
            t.errors_since_log = 0;
        }
    }

    Buf helloFrame() const
    {
        return share(skycue::encodeHello(
            skycue::Hello{toMs32(keyframe_interval), toMs32(heartbeat_interval), cfg.sender_name}));
    }

    // UDP has no connection to greet, so every target gets a Hello at start()
    // and before each keyframe burst. Not a broadcast: TCP clients got theirs
    // on accept, and it does not hold back the heartbeat.
    void helloToUdp()
    {
        if (targets.empty())
            return;
        const Buf b = helloFrame();
        for (auto& t : targets)
            sendUdp(t, b);
    }

    std::vector<Buf> keyframeFrames()
    {
        std::vector<Buf> out;
        if (!provider)
            return out;
        auto tracks = provider();
        out.reserve(tracks.size());
        for (auto& t : tracks) {
            t.keyframe = true;
            out.push_back(share(skycue::encodeTrackUpdate(t)));
        }
        return out;
    }

    // ---- timers --------------------------------------------------------------

    void armKeyframe()
    {
        const unsigned gen = ++keyframe_gen;
        keyframe_timer.expires_at(next_keyframe);
        auto self = shared_from_this();
        keyframe_timer.async_wait([self, gen](const error_code& ec) {
            if (ec || self->stopping || gen != self->keyframe_gen)
                return;
            self->helloToUdp(); // so UDP receivers know the keyframe interval before the burst
            for (const Buf& b : self->keyframeFrames())
                self->broadcast(b);
            const auto now = steady::now();
            self->next_keyframe += self->keyframe_interval;
            if (self->next_keyframe <= now) // fell behind: do not burst to catch up
                self->next_keyframe = now + self->keyframe_interval;
            self->armKeyframe();
        });
    }

    void armHeartbeat()
    {
        const unsigned gen = ++heartbeat_gen;
        heartbeat_timer.expires_at(last_broadcast + heartbeat_interval);
        auto self = shared_from_this();
        heartbeat_timer.async_wait([self, gen](const error_code& ec) {
            if (ec || self->stopping || gen != self->heartbeat_gen)
                return;
            if (steady::now() - self->last_broadcast >= self->heartbeat_interval) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch());
                self->broadcast(share(skycue::encodeHeartbeat(skycue::Heartbeat{static_cast<uint64_t>(ms.count())})));
            }
            self->armHeartbeat(); // due again heartbeat_interval after the latest broadcast
        });
    }

    // ---- stop ----------------------------------------------------------------

    void stop()
    {
        if (stopping)
            return;
        const bool was_started = started;
        stopping = true;
        ++keyframe_gen;
        ++heartbeat_gen;
        ++accept_gen;
        error_code ignored;
        keyframe_timer.cancel();
        heartbeat_timer.cancel();
        accept_retry_timer.cancel();
        acceptor.close(ignored);
        cancelLinks();
        if (!was_started)
            return;

        if (provider) {
            const auto tracks = provider();
            for (const auto& t : tracks)
                broadcast(share(skycue::encodeTrackDrop(skycue::TrackDrop{t.id, skycue::DropReason::shutdown})));
        }
        LOG_INFO("forwarder: stopping, flushing " << sessions.size() << " TCP client(s)");
        std::vector<std::shared_ptr<Session>> all;
        for (const auto& kv : sessions)
            all.push_back(kv.second);
        for (const auto& s : all) {
            s->close_when_drained = true;
            if (s->inflight.empty())
                write(s); // nothing in flight: closes now
        }
        // Bound the flush: a client that does not read must not hold up exit.
        // (Outbound connections drain the same way and are not redialled.)
        auto self = shared_from_this();
        stop_timer.expires_after(kStopDrainLimit);
        stop_timer.async_wait([self](const error_code& ec) {
            if (!ec)
                self->closeAll();
        });
        maybeFinishStop();
    }

    void maybeFinishStop()
    {
        if (!stopping || dead || !sessions.empty() || udp_inflight != 0)
            return;
        closeAll();
    }

    // Closes every socket and timer at once; pending handlers then run with
    // operation_aborted and do nothing.
    void closeAll()
    {
        if (dead)
            return;
        dead = true;
        stopping = true;
        ++keyframe_gen;
        ++heartbeat_gen;
        ++accept_gen;
        error_code ignored;
        keyframe_timer.cancel();
        heartbeat_timer.cancel();
        accept_retry_timer.cancel();
        stop_timer.cancel();
        acceptor.close(ignored);
        cancelLinks();
        udp_sock.close(ignored);
        std::vector<std::shared_ptr<Session>> all;
        for (const auto& kv : sessions)
            all.push_back(kv.second);
        for (const auto& s : all) {
            s->closed = true;
            s->sock.close(ignored);
        }
        sessions.clear();
    }
};

Forwarder::Forwarder(boost::asio::io_context& io, ForwarderConfig config)
    : impl_(std::make_shared<Impl>(io, std::move(config)))
{
}

// NOLINTNEXTLINE(bugprone-exception-escape): a throw here is a broken process (asio's throwing cancel or close, a failed join), and terminate is the right end for it
Forwarder::~Forwarder() { impl_->closeAll(); }

void Forwarder::start() { impl_->start(); }
void Forwarder::stop() { impl_->stop(); }

void Forwarder::sendUpdate(const skycue::Track& track)
{
    if (!impl_->started || impl_->stopping)
        return;
    impl_->broadcast(share(skycue::encodeTrackUpdate(track)));
}

void Forwarder::sendDrop(skycue::TrackId id, skycue::DropReason reason)
{
    if (!impl_->started || impl_->stopping)
        return;
    impl_->broadcast(share(skycue::encodeTrackDrop(skycue::TrackDrop{id, reason})));
}

void Forwarder::setKeyframeProvider(KeyframeProvider provider) { impl_->provider = std::move(provider); }
size_t Forwarder::tcpClientCount() const { return impl_->sessions.size(); }
size_t Forwarder::udpTargetCount() const { return impl_->targets.size(); }

std::vector<CueTcpClient> Forwarder::tcpClients() const
{
    std::vector<CueTcpClient> out;
    out.reserve(impl_->sessions.size());
    for (const auto& kv : impl_->sessions) {
        const Impl::Session& s = *kv.second;
        out.push_back(CueTcpClient{s.peer, s.link != nullptr, s.connected_since, s.bytes_sent});
    }
    return out;
}

std::vector<CueUdpTarget> Forwarder::udpTargets() const
{
    std::vector<CueUdpTarget> out;
    out.reserve(impl_->targets.size());
    for (const auto& t : impl_->targets)
        out.push_back(CueUdpTarget{t.name, t.datagrams_sent});
    return out;
}
uint64_t Forwarder::framesSent() const { return impl_->frames_sent; }
uint16_t Forwarder::tcpPort() const { return impl_->port; }

} // namespace skycued
