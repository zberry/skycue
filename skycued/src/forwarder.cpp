// Forwarder. Single-threaded: everything runs on the caller's io_context
// thread, so there are no locks. The sockets are skycue::net Listener, Dialer
// and UdpSender objects; one Fanout publishes every frame to all of them.
// This file keeps the protocol (Hello, keyframe bursts, heartbeats, shutdown
// drops) and the session list the Cues tab reads.
//
//
// Peer events come from the Fanout (onPeerUp/onPeerDown/peerInfo); the Forwarder
// never touches a Listener's or Dialer's own handlers. Log lines about
// connection failures (a dial that is refused, a bind that fails and is
// retried) now come from the library through the net log sink, once per
// outage, not from this file.

#include "forwarder.hpp"

#include "log.hpp"

#include <skycue/net/dialer.hpp>
#include <skycue/net/endpoint.hpp>
#include <skycue/net/fanout.hpp>
#include <skycue/net/listener.hpp>
#include <skycue/net/udp_sender.hpp>
#include <skycue/protocol.hpp>

#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <map>
#include <stdexcept>

namespace skycued {

namespace asio = boost::asio;
using error_code = boost::system::error_code;
using steady = std::chrono::steady_clock;
using skycue::net::Buf;
using skycue::net::Transport;

namespace {
constexpr std::chrono::milliseconds kMinInterval{10};
constexpr std::chrono::seconds kStopDrainLimit{2};
constexpr std::chrono::milliseconds kDrainPoll{20};
constexpr size_t kMaxPeers = 1024; // the library's default (16) would be a new limit for the cue port

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
        fanout.onPeerUp([this](uint64_t peer, const boost::asio::ip::tcp::endpoint& who,
                               const std::function<void(const Buf&)>& send) { peerUp(peer, who, send); });
        fanout.onPeerDown([this](uint64_t peer) { peerDown(peer); });
        if (cfg.tcp_listen_ephemeral || cfg.tcp_port != 0) {
            skycue::net::Endpoint e;
            e.transport = Transport::tcp_listen;
            e.bind = "0.0.0.0";
            e.port = cfg.tcp_listen_ephemeral ? uint16_t{0} : cfg.tcp_port;
            dests.push_back(std::make_shared<Dest>(ForwarderDestination{"cue-tcp", e, true}));
        }
        size_t n = 0;
        for (const auto& t : cfg.tcp_connect_targets) {
            skycue::net::Endpoint e;
            e.transport = Transport::tcp_connect;
            e.host = t.first;
            e.port = t.second;
            dests.push_back(std::make_shared<Dest>(ForwarderDestination{"cue-connect-" + std::to_string(++n), e, true}));
        }
        n = 0;
        for (const auto& t : cfg.udp_targets) {
            skycue::net::Endpoint e;
            e.transport = Transport::udp;
            e.host = t.first;
            e.port = t.second;
            dests.push_back(std::make_shared<Dest>(ForwarderDestination{"cue-udp-" + std::to_string(++n), e, true}));
        }
    }

    // One destination and, once started, its library object.
    struct Dest {
        explicit Dest(ForwarderDestination d_) : d(std::move(d_)) {}
        ForwarderDestination d;
        std::shared_ptr<skycue::net::Listener> lis;
        std::shared_ptr<skycue::net::Dialer> dial;
        std::shared_ptr<skycue::net::UdpSender> udp;
        uint64_t dropped_seen = 0;

        std::string name() const
        {
            const auto& e = d.endpoint;
            if (e.transport == Transport::tcp_listen)
                return e.bind + ":" + std::to_string(e.port);
            return (e.host.empty() ? e.group : e.host) + ":" + std::to_string(e.port);
        }
        const void* source() const { return lis ? static_cast<const void*>(lis.get()) : static_cast<const void*>(dial.get()); }
        const skycue::net::Counters& counters() const { return lis ? lis->counters() : dial->counters(); }
    };

    // One open TCP connection, accepted or dialled.
    struct Session {
        std::string peer;
        bool dialled = false;
        std::chrono::system_clock::time_point connected_since{};
        const void* source = nullptr; // the library object (Fanout::PeerInfo::source) it belongs to
    };

    asio::io_context& io;
    ForwarderConfig cfg;
    std::chrono::milliseconds keyframe_interval, heartbeat_interval;
    KeyframeProvider provider;

    skycue::net::Fanout fanout;
    std::vector<std::shared_ptr<Dest>> dests;
    std::map<uint64_t, Session> sessions;
    uint64_t next_session_id = 1;

    asio::steady_timer keyframe_timer{io};
    asio::steady_timer heartbeat_timer{io};
    asio::steady_timer drain_timer{io};

    bool started = false, stopping = false, dead = false;
    unsigned keyframe_gen = 0, heartbeat_gen = 0, drain_gen = 0;
    steady::time_point next_keyframe{}, last_broadcast{}, drain_deadline{};
    uint64_t frames_sent = 0;

    skycue::net::TcpOptions tcpOptions(const std::string& id) const
    {
        skycue::net::TcpOptions o; // keepalive and nodelay on
        o.name = id;
        o.send_cap = cfg.max_client_queue_bytes;
        return o;
    }

    Dest* find(const std::string& id) const
    {
        for (const auto& d : dests)
            if (d->d.id == id)
                return d.get();
        return nullptr;
    }

    // ---- start -----------------------------------------------------------

    void start()
    {
        if (started || stopping)
            return;
        const bool listening = find("cue-tcp") != nullptr;
        try {
            for (const auto& d : dests)
                activate(*d);
        } catch (const std::runtime_error& e) {
            closeDests();
            throw std::runtime_error("forwarder: cannot listen on TCP port " + std::to_string(cfg.tcp_port) + ": " +
                                     e.what());
        }
        started = true;
        if (listening)
            LOG_INFO("forwarder: listening on TCP port " << tcpPort());
        else
            LOG_INFO("forwarder: TCP listener disabled (CUE_TCP_PORT=0)");
        helloToUdp();

        const auto now = steady::now();
        last_broadcast = now;
        next_keyframe = now + keyframe_interval;
        armKeyframe();
        armHeartbeat();
    }

    uint16_t tcpPort() const
    {
        const Dest* d = find("cue-tcp");
        return d && d->lis ? d->lis->localPort() : uint16_t{0};
    }

    // Creates and starts the library object of one destination. A listener
    // with fail_fast (the built-in port) throws std::runtime_error.
    void activate(Dest& d)
    {
        const auto& e = d.d.endpoint;
        if (e.transport == Transport::tcp_listen) {
            skycue::net::ListenOptions lo;
            lo.tcp = tcpOptions(d.d.id);
            lo.max_peers = kMaxPeers;
            lo.peer = e.peer;
            lo.fail_fast = d.d.builtin;
            d.lis = skycue::net::Listener::create(io, e.bind, e.port, lo);
            fanout.attach(d.lis);
            try {
                d.lis->start();
            } catch (...) {
                fanout.detach(d.lis);
                d.lis->stop();
                d.lis.reset();
                throw;
            }
            if (!d.d.builtin)
                LOG_INFO("forwarder: TCP listener " << d.name());
        } else if (e.transport == Transport::tcp_connect) {
            d.dial = skycue::net::Dialer::create(io, e.host, e.port, tcpOptions(d.d.id));
            // Attached before start(): the Fanout's peer-up runs inside the
            // connect handler, so the Hello is queued before anything published later.
            fanout.attach(d.dial);
            LOG_INFO("forwarder: TCP target " << d.name());
            d.dial->start();
        } else {
            d.udp = skycue::net::UdpSender::create(io, skycue::kHeaderSize + skycue::kMaxUdpPayload, d.d.id);
            skycue::net::UdpTarget t;
            t.host = e.group.empty() ? e.host : e.group;
            t.port = e.port;
            t.iface = e.iface;
            t.ttl = e.ttl;
            t.loopback = e.loopback;
            d.udp->addTarget(t);
            if (d.udp->start() == 0) {
                LOG_ERROR("forwarder: UDP target " << d.name() << " skipped");
                d.udp.reset();
                return;
            }
            fanout.attach(d.udp);
            LOG_INFO("forwarder: UDP target " << d.name());
        }
    }

    void deactivate(Dest& d)
    {
        const void* src = d.source();
        if (d.lis) {
            fanout.detach(d.lis);
            d.lis->stop();
            d.lis.reset();
        }
        if (d.dial) {
            fanout.detach(d.dial);
            d.dial->stop();
            d.dial.reset();
        }
        if (d.udp) {
            fanout.detach(d.udp);
            d.udp->stop();
            d.udp.reset();
        }
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (src != nullptr && it->second.source == src)
                it = sessions.erase(it);
            else
                ++it;
        }
    }

    void closeDests()
    {
        for (const auto& d : dests)
            deactivate(*d);
    }

    // ---- TCP sessions ------------------------------------------------------

    Dest* destOf(const void* source) const
    {
        for (const auto& d : dests)
            if (source != nullptr && d->source() == source)
                return d.get();
        return nullptr;
    }

    void peerUp(uint64_t peer, const boost::asio::ip::tcp::endpoint& who, const std::function<void(const Buf&)>& send)
    {
        if (dead || stopping)
            return;
        const auto info = fanout.peerInfo(peer);
        if (!info)
            return;
        Dest* d = destOf(info->source);
        Session s;
        s.dialled = info->dialled;
        s.peer = info->dialled && d ? d->name()
                                    : endpointText(skycue::net::unmapV4(who.address()).to_string(), who.port());
        s.connected_since = std::chrono::system_clock::now();
        s.source = info->source;
        const auto& stored = sessions.emplace(peer, std::move(s)).first->second;
        LOG_INFO("forwarder: " << (stored.dialled ? "TCP target " : "TCP client ") << stored.peer << " connected ("
                               << sessions.size() << " total)");
        greet(peer, send);
    }

    void peerDown(uint64_t peer)
    {
        const auto it = sessions.find(peer);
        if (it == sessions.end())
            return;
        const std::string name = it->second.peer;
        const bool target = it->second.dialled;
        Dest* d = destOf(it->second.source);
        sessions.erase(it);
        bool over_cap = false;
        if (d) {
            const uint64_t dropped = d->lis ? d->lis->counters().dropped : d->dial->counters().dropped;
            over_cap = dropped > d->dropped_seen;
            d->dropped_seen = dropped;
        }
        const char* kind = target ? "TCP target " : "TCP client ";
        if (over_cap)
            LOG_WARN("forwarder: " << kind << name << " closed: outbound queue exceeded " << cfg.max_client_queue_bytes
                                   << " bytes");
        else
            LOG_INFO("forwarder: " << kind << name << " disconnected (" << sessions.size() << " left)");
        drainCheck();
    }

    // Hello, then one keyframe per provider track, to that peer only.
    void greet(uint64_t peer, const std::function<void(const Buf&)>& send)
    {
        ++frames_sent;
        send(helloFrame());
        for (const Buf& b : keyframeFrames()) {
            if (!fanout.peerInfo(peer))
                break; // cut off (queue cap) while greeting
            ++frames_sent;
            send(b);
        }
    }

    // ---- broadcast ---------------------------------------------------------

    void broadcast(const Buf& b)
    {
        last_broadcast = steady::now();
        frames_sent += sessions.size();
        if (b->size() <= skycue::kHeaderSize + skycue::kMaxUdpPayload)
            frames_sent += udpTargetCount();
        fanout.publish(b);
    }

    size_t udpTargetCount() const
    {
        return static_cast<size_t>(
            std::count_if(dests.begin(), dests.end(), [](const auto& d) { return d->udp != nullptr; }));
    }

    Buf helloFrame() const
    {
        return share(skycue::encodeHello(
            skycue::Hello{toMs32(keyframe_interval), toMs32(heartbeat_interval), cfg.sender_name}));
    }

    void helloTo(Dest& d)
    {
        if (!d.udp)
            return;
        ++frames_sent;
        d.udp->send(helloFrame());
    }

    // UDP has no connection to greet, so every target gets a Hello at start()
    // and before each keyframe burst. Not a broadcast: TCP clients got theirs
    // on connect, and it does not hold back the heartbeat.
    void helloToUdp()
    {
        for (const auto& d : dests)
            helloTo(*d);
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

    // ---- runtime destinations ----------------------------------------------

    void add(const ForwarderDestination& d)
    {
        if (dead || stopping)
            return;
        const auto& e = d.endpoint;
        if (d.id.empty() || find(d.id) != nullptr)
            throw std::invalid_argument("forwarder: destination id empty or already in use: " + d.id);
        if (e.port == 0 && e.transport != Transport::tcp_listen)
            throw std::invalid_argument("forwarder: destination " + d.id + " has no port");
        if (e.transport == Transport::tcp_connect && e.host.empty())
            throw std::invalid_argument("forwarder: destination " + d.id + " has no host");
        if (e.transport == Transport::udp && e.host.empty() && e.group.empty())
            throw std::invalid_argument("forwarder: destination " + d.id + " has no host or group");
        auto dest = std::make_shared<Dest>(d);
        if (started) {
            try {
                activate(*dest);
            } catch (const std::runtime_error&) {
                deactivate(*dest);
                throw;
            }
        }
        dests.push_back(dest);
        if (started)
            helloTo(*dest);
    }

    void remove(const std::string& id)
    {
        for (auto it = dests.begin(); it != dests.end(); ++it) {
            if ((*it)->d.id != id)
                continue;
            deactivate(**it);
            LOG_INFO("forwarder: destination " << id << " removed");
            dests.erase(it);
            drainCheck();
            return;
        }
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
        keyframe_timer.cancel();
        heartbeat_timer.cancel();
        if (!was_started) {
            closeAll();
            return;
        }

        if (provider) {
            const auto tracks = provider();
            for (const auto& t : tracks)
                broadcast(share(skycue::encodeTrackDrop(skycue::TrackDrop{t.id, skycue::DropReason::shutdown})));
        }
        LOG_INFO("forwarder: stopping, flushing " << sessions.size() << " TCP client(s)");
        // Bound the flush: a client that does not read must not hold up exit.
        drain_deadline = steady::now() + kStopDrainLimit;
        drainCheck();
    }

    // True when no live TCP peer has bytes left in its queue. A peer that was
    // cut off is gone from the Fanout and counts as drained.
    bool drained() const
    {
        for (const auto& kv : sessions) {
            const auto info = fanout.peerInfo(kv.first);
            if (info && info->stats.queued_bytes != 0)
                return false;
        }
        return true;
    }

    // While stopping: closes everything once the queues are written or the
    // limit has passed, else looks again shortly.
    void drainCheck()
    {
        if (!stopping || dead || !started)
            return;
        if (sessions.empty() || drained() || steady::now() >= drain_deadline) {
            closeAll();
            return;
        }
        const unsigned gen = ++drain_gen;
        auto self = shared_from_this();
        drain_timer.expires_after(kDrainPoll);
        drain_timer.async_wait([self, gen](const error_code& ec) {
            if (!ec && gen == self->drain_gen)
                self->drainCheck();
        });
    }

    // Closes every socket and timer at once.
    void closeAll()
    {
        if (dead)
            return;
        dead = true;
        stopping = true;
        ++keyframe_gen;
        ++heartbeat_gen;
        ++drain_gen;
        keyframe_timer.cancel();
        heartbeat_timer.cancel();
        drain_timer.cancel();
        closeDests();
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
void Forwarder::addDestination(const ForwarderDestination& d) { impl_->add(d); }
void Forwarder::removeDestination(const std::string& id) { impl_->remove(id); }

std::vector<ForwarderDestination> Forwarder::destinations() const
{
    std::vector<ForwarderDestination> out;
    out.reserve(impl_->dests.size());
    for (const auto& d : impl_->dests)
        out.push_back(d->d);
    return out;
}

size_t Forwarder::tcpClientCount() const { return impl_->sessions.size(); }
size_t Forwarder::udpTargetCount() const { return impl_->udpTargetCount(); }

std::vector<CueTcpClient> Forwarder::tcpClients() const
{
    std::vector<CueTcpClient> out;
    out.reserve(impl_->sessions.size());
    for (const auto& kv : impl_->sessions) {
        const Impl::Session& s = kv.second;
        const auto info = impl_->fanout.peerInfo(kv.first);
        out.push_back(CueTcpClient{s.peer, s.dialled, s.connected_since, info ? info->stats.bytes_written : 0});
    }
    return out;
}

std::vector<CueUdpTarget> Forwarder::udpTargets() const
{
    std::vector<CueUdpTarget> out;
    for (const auto& d : impl_->dests)
        if (d->udp)
            out.push_back(CueUdpTarget{d->name(), d->udp->counters().messages_out});
    return out;
}

uint64_t Forwarder::framesSent() const { return impl_->frames_sent; }
uint16_t Forwarder::tcpPort() const { return impl_->tcpPort(); }

} // namespace skycued
