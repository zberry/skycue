// skycue client. Everything below the owner-side functions (start, stop,
// poll, trackCount, localPort) runs on the I/O thread only: the thread start()
// spawns, or the thread calling poll(). The one exception is the first
// tcp_listen bind, which start() does before there is an I/O thread, so that
// localPort() is known when it returns. Asio API kept to what Boost 1.66 offers.

#include <skycue/client.hpp>
#include <skycue/protocol.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <map>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skycue {

namespace asio = boost::asio;
using asio::ip::tcp;
using asio::ip::udp;
using error_code = boost::system::error_code;
using steady = std::chrono::steady_clock;

namespace {
constexpr std::chrono::milliseconds kMinKeyframeInterval{10};
} // namespace

struct Client::Impl {
    explicit Impl(Config c) : cfg(std::move(c)), keyframe_interval(clampInterval(cfg.default_keyframe_interval)) {}

    static std::chrono::milliseconds clampInterval(std::chrono::milliseconds ms)
    {
        return std::max(ms, kMinKeyframeInterval);
    }

    struct Entry {
        Track track;
        steady::time_point last_keyframe; // or first appearance
    };

    Config cfg;
    std::function<void(const Track&)> on_update;
    std::function<void(TrackId, DropReason)> on_dropped;
    std::function<void(bool)> on_connection;
    std::function<void(const Frame&)> on_frame;

    // Declared first so it is destroyed last, after every I/O object.
    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard;
    std::thread thread;

    tcp::resolver resolver{io};
    tcp::socket tcp_sock{io};
    udp::socket udp_sock{io};
    asio::steady_timer reconnect_timer{io};
    asio::steady_timer stale_timer{io};
    asio::steady_timer heartbeat_timer{io};

    std::vector<uint8_t> rx;
    std::array<uint8_t, 4096> chunk{};

    // tcp_listen: one Peer per accepted connection, each with its own decoder
    // state. Handlers hold a shared_ptr to their Peer; `open` turns them off.
    struct Peer {
        Peer(asio::io_context& io, unsigned id_) : sock(io), id(id_) {}
        tcp::socket sock;
        unsigned id;
        bool open = true;
        std::vector<uint8_t> rx;
        std::array<uint8_t, 4096> chunk{};
    };
    tcp::acceptor acceptor{io};
    std::map<unsigned, std::shared_ptr<Peer>> peers;
    unsigned next_peer = 0, accept_gen = 0;
    std::atomic<uint16_t> local_port{0};
    std::array<uint8_t, kHeaderSize + kMaxUdpPayload + 1> dgram{}; // +1 so an oversized datagram shows
    udp::endpoint from;

    std::unordered_map<TrackId, Entry> tracks;
    std::atomic<size_t> count{0};

    // I/O-thread state. A handler whose generation no longer matches belongs
    // to a cancelled operation or a closed connection and does nothing.
    bool connected = false;
    bool stopping = false;
    std::chrono::milliseconds keyframe_interval;
    steady::time_point last_frame{};
    unsigned conn_gen = 0, reconnect_gen = 0, stale_gen = 0, heartbeat_gen = 0;

    // First exception thrown by a user callback; rethrown from poll() in poll
    // mode, discarded in thread mode.
    std::exception_ptr callback_error;

    // Owner-side state.
    bool started = false;
    std::atomic<bool> stop_requested{false};
    // Set by the I/O thread itself, so reading it never races with the
    // assignment of `thread`. A default id matches no running thread.
    std::atomic<std::thread::id> io_thread_id{};

    bool onIoThread() const { return std::this_thread::get_id() == io_thread_id.load(); }

    // ---- user callbacks: the only call sites ------------------------------
    // A throwing callback must not break the async chains, so every call is
    // contained here and the caller carries on as if it had returned.

    template <class F> void guarded(F&& f)
    {
        try {
            f();
        } catch (...) {
            if (!cfg.run_io_thread && !callback_error)
                callback_error = std::current_exception();
        }
    }
    void fireUpdate(const Track& t)
    {
        if (on_update)
            guarded([&] { on_update(t); });
    }
    void fireDropped(TrackId id, DropReason r)
    {
        if (on_dropped)
            guarded([&] { on_dropped(id, r); });
    }
    void fireConnection(bool up)
    {
        if (on_connection)
            guarded([&] { on_connection(up); });
    }
    void fireFrame(const Frame& f)
    {
        if (on_frame)
            guarded([&] { on_frame(f); });
    }

    // ---- I/O thread ------------------------------------------------------

    void begin()
    {
        if (stopping)
            return;
        armStale();
        openTransport();
    }

    void openTransport()
    {
        switch (cfg.transport) {
        case Config::Transport::tcp:
            connectTcp();
            return;
        case Config::Transport::tcp_listen:
            listenTcp();
            return;
        case Config::Transport::udp:
        default:
            bindUdp();
            return;
        }
    }

    void scheduleReconnect()
    {
        if (stopping)
            return;
        const unsigned gen = ++reconnect_gen;
        reconnect_timer.expires_after(cfg.reconnect_interval);
        reconnect_timer.async_wait([this, gen](const error_code& ec) {
            if (stopping || ec || gen != reconnect_gen)
                return;
            openTransport();
        });
    }

    void connectTcp()
    {
        const unsigned gen = ++conn_gen;
        resolver.async_resolve(cfg.host, std::to_string(cfg.port),
                               [this, gen](const error_code& ec, tcp::resolver::results_type results) {
                                   if (stopping || gen != conn_gen)
                                       return;
                                   if (ec) {
                                       scheduleReconnect();
                                       return;
                                   }
                                   asio::async_connect(tcp_sock, results,
                                                       [this, gen](const error_code& cec, const tcp::endpoint&) {
                                                           if (stopping || gen != conn_gen)
                                                               return;
                                                           if (cec) {
                                                               closeTcp();
                                                               scheduleReconnect();
                                                               return;
                                                           }
                                                           rx.clear();
                                                           setConnected(true);
                                                           if (!stopping && gen == conn_gen)
                                                               readTcp(gen);
                                                       });
                               });
    }

    void closeTcp()
    {
        error_code ignored;
        tcp_sock.close(ignored);
        rx.clear();
    }

    void lostTcp()
    {
        ++conn_gen;
        closeTcp();
        setConnected(false);
        scheduleReconnect();
    }

    void readTcp(unsigned gen)
    {
        tcp_sock.async_read_some(asio::buffer(chunk), [this, gen](const error_code& ec, size_t n) {
            if (stopping || gen != conn_gen)
                return;
            if (ec) {
                lostTcp();
                return;
            }
            rx.insert(rx.end(), chunk.data(), chunk.data() + n);
            const bool good = consumeFrames(rx, [&] { return !stopping && gen == conn_gen; });
            if (stopping || gen != conn_gen) // a callback stopped the client
                return;
            if (!good) { // bad_magic / bad_version: cannot resynchronise
                lostTcp();
                return;
            }
            readTcp(gen);
        });
    }

    // Consumes the decodable frames at the front of `buf` (a stream's
    // reassembly buffer), erasing them. False on a bad magic or version: the
    // stream cannot be resynchronised. `alive` says whether to go on (a
    // callback may have stopped the client or closed the connection).
    template <class Alive> bool consumeFrames(std::vector<uint8_t>& buf, Alive alive)
    {
        size_t offset = 0;
        bool good = true;
        while (alive()) {
            Frame frame;
            size_t used = 0;
            const DecodeStatus st = decodeFrame(buf.data() + offset, buf.size() - offset, frame, used);
            if (st == DecodeStatus::need_more)
                break;
            if (st != DecodeStatus::ok) {
                good = false;
                break;
            }
            offset += used;
            handleFrame(frame);
        }
        // Bounded by construction: what is left is a partial frame, less
        // than kHeaderSize + 0xFFFF bytes (the u16 length field caps a
        // frame), so a buffer never exceeds that plus one read chunk. Garbage
        // fails the magic/version check as soon as 8 bytes are buffered.
        if (good && alive())
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(offset));
        return good;
    }

    // ---- tcp_listen --------------------------------------------------------

    // Binds and listens if not already listening. Called once from start()
    // (owner thread, before the I/O thread exists) and on the I/O thread for
    // retries.
    bool openListener()
    {
        if (acceptor.is_open())
            return true;
        error_code ec;
        tcp::endpoint ep;
        const auto addr = asio::ip::make_address(cfg.host, ec);
        if (!ec) {
            ep = tcp::endpoint(addr, cfg.port);
        } else {
            tcp::resolver r(io);
            const auto results = r.resolve(cfg.host, std::to_string(cfg.port), ec);
            if (ec || results.empty())
                return false;
            ep = *results.begin();
        }
        acceptor.open(ep.protocol(), ec);
        if (!ec)
            acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
        if (!ec)
            acceptor.bind(ep, ec);
        if (!ec)
            acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (!ec)
            local_port = acceptor.local_endpoint(ec).port();
        if (ec) {
            error_code ignored;
            acceptor.close(ignored);
            local_port = 0;
            return false;
        }
        return true;
    }

    void listenTcp()
    {
        if (!openListener()) {
            scheduleReconnect(); // retry the bind
            return;
        }
        acceptNext();
    }

    void acceptNext()
    {
        auto p = std::make_shared<Peer>(io, ++next_peer);
        const unsigned gen = ++accept_gen;
        acceptor.async_accept(p->sock, [this, p, gen](const error_code& ec) {
            if (stopping || gen != accept_gen)
                return;
            if (ec) {
                // EMFILE and the like: keep the listener, try again later.
                if (ec != asio::error::operation_aborted)
                    scheduleReconnect();
                return;
            }
            peers.emplace(p->id, p);
            setConnected(true); // fires for the first connection only
            if (stopping)
                return;
            readPeer(p);
            acceptNext();
        });
    }

    void readPeer(const std::shared_ptr<Peer>& p)
    {
        p->sock.async_read_some(asio::buffer(p->chunk), [this, p](const error_code& ec, size_t n) {
            if (stopping || !p->open)
                return;
            if (ec) {
                closePeer(p);
                return;
            }
            p->rx.insert(p->rx.end(), p->chunk.data(), p->chunk.data() + n);
            const bool good = consumeFrames(p->rx, [&] { return !stopping && p->open; });
            if (stopping || !p->open)
                return;
            if (!good) {
                closePeer(p);
                return;
            }
            readPeer(p);
        });
    }

    void closePeer(const std::shared_ptr<Peer>& p)
    {
        if (!p->open)
            return;
        p->open = false;
        error_code ignored;
        p->sock.close(ignored);
        p->rx.clear();
        peers.erase(p->id);
        if (peers.empty())
            setConnected(false); // the last source is gone: source_lost
    }

    // ---- udp ---------------------------------------------------------------

    void bindUdp()
    {
        error_code ec;
        udp::endpoint ep;
        const auto addr = asio::ip::make_address(cfg.host, ec);
        if (!ec) {
            ep = udp::endpoint(addr, cfg.port);
        } else {
            udp::resolver r(io);
            const auto results = r.resolve(cfg.host, std::to_string(cfg.port), ec);
            if (ec || results.empty()) {
                scheduleReconnect();
                return;
            }
            ep = *results.begin();
        }
        udp_sock.open(ep.protocol(), ec);
        if (!ec)
            udp_sock.bind(ep, ec);
        if (ec) {
            error_code ignored;
            udp_sock.close(ignored);
            scheduleReconnect();
            return;
        }
        receiveUdp();
    }

    void receiveUdp()
    {
        udp_sock.async_receive_from(asio::buffer(dgram), from, [this](const error_code& ec, size_t n) {
            if (stopping || ec == asio::error::operation_aborted)
                return;
            if (ec) { // rebind after a socket error
                error_code ignored;
                udp_sock.close(ignored);
                scheduleReconnect();
                return;
            }
            Frame frame;
            size_t used = 0;
            // Exactly one frame per datagram: drop it unless its size is
            // 8 + length. The buffer is one byte larger than the largest legal
            // datagram, so a longer one arrives truncated to that size and is
            // caught by the upper bound.
            if (decodeFrame(dgram.data(), n, frame, used) == DecodeStatus::ok && used == n &&
                used <= kHeaderSize + kMaxUdpPayload) {
                last_frame = steady::now();
                if (!connected) {
                    setConnected(true);
                    if (!stopping)
                        armHeartbeat(last_frame + cfg.heartbeat_timeout);
                }
                if (!stopping)
                    handleFrame(frame);
            }
            if (!stopping)
                receiveUdp();
        });
    }

    void armHeartbeat(steady::time_point deadline)
    {
        const unsigned gen = ++heartbeat_gen;
        heartbeat_timer.expires_at(deadline);
        heartbeat_timer.async_wait([this, gen](const error_code& ec) {
            if (stopping || ec || gen != heartbeat_gen || !connected)
                return;
            const auto due = last_frame + cfg.heartbeat_timeout;
            if (steady::now() >= due)
                setConnected(false);
            else
                armHeartbeat(due);
        });
    }

    void armStale()
    {
        const unsigned gen = ++stale_gen;
        stale_timer.expires_after(keyframe_interval * 3 / 2);
        stale_timer.async_wait([this, gen](const error_code& ec) {
            if (stopping || ec || gen != stale_gen)
                return;
            dropStale();
            if (!stopping && gen == stale_gen)
                armStale();
        });
    }

    void dropStale()
    {
        const auto now = steady::now();
        const auto limit = keyframe_interval * 2;
        std::vector<TrackId> stale;
        for (const auto& kv : tracks)
            if (now - kv.second.last_keyframe > limit)
                stale.push_back(kv.first);
        for (const auto& id : stale) {
            if (stopping)
                return;
            tracks.erase(id);
            count = tracks.size();
            fireDropped(id, DropReason::stale);
        }
    }

    void setConnected(bool up)
    {
        if (connected == up)
            return;
        connected = up;
        fireConnection(up);
        if (!up)
            dropAll(DropReason::source_lost);
    }

    void dropAll(DropReason reason)
    {
        auto old = std::move(tracks);
        tracks.clear();
        count = 0;
        for (const auto& kv : old) {
            if (stopping)
                return;
            fireDropped(kv.first, reason);
        }
    }

    void handleFrame(const Frame& frame)
    {
        fireFrame(frame);
        if (stopping) // the callback stopped the client
            return;
        switch (frame.type) {
        case FrameType::hello: {
            Hello h;
            if (!decodeHello(frame, h) || h.keyframe_interval_ms == 0)
                return;
            const auto interval = clampInterval(std::chrono::milliseconds(h.keyframe_interval_ms));
            if (interval != keyframe_interval) {
                keyframe_interval = interval;
                armStale();
            }
            return;
        }
        case FrameType::track_update: {
            Track t;
            if (!decodeTrackUpdate(frame, t))
                return;
            const auto now = steady::now();
            auto it = tracks.find(t.id);
            if (it == tracks.end()) {
                tracks.emplace(t.id, Entry{t, now});
                count = tracks.size();
            } else {
                it->second.track = t;
                if (t.keyframe) // deltas do not refresh the stale clock
                    it->second.last_keyframe = now;
            }
            fireUpdate(t);
            return;
        }
        case FrameType::track_drop: {
            TrackDrop d;
            if (!decodeTrackDrop(frame, d))
                return;
            if (tracks.erase(d.id) == 0)
                return;
            count = tracks.size();
            fireDropped(d.id, d.reason);
            return;
        }
        case FrameType::heartbeat: // liveness only; UDP accounting is done per frame
        default:
            return;
        }
    }

    void shutdownOnIo()
    {
        stopping = true;
        ++conn_gen;
        ++reconnect_gen;
        ++stale_gen;
        ++heartbeat_gen;
        ++accept_gen;
        reconnect_timer.cancel();
        stale_timer.cancel();
        heartbeat_timer.cancel();
        resolver.cancel();
        error_code ignored;
        tcp_sock.close(ignored);
        udp_sock.close(ignored);
        acceptor.close(ignored);
        for (auto& kv : peers) {
            kv.second->open = false;
            kv.second->sock.close(ignored);
        }
        peers.clear();
        rx.clear();
        tracks.clear();
        count = 0;
        connected = false;
    }
};

Client::Client(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Client::~Client() { stop(); }

void Client::onTrackUpdate(std::function<void(const Track&)> cb) { impl_->on_update = std::move(cb); }
void Client::onTrackDropped(std::function<void(TrackId, DropReason)> cb) { impl_->on_dropped = std::move(cb); }
void Client::onConnectionChange(std::function<void(bool)> cb) { impl_->on_connection = std::move(cb); }
void Client::onFrame(std::function<void(const Frame&)> cb) { impl_->on_frame = std::move(cb); }

void Client::start()
{
    Impl& d = *impl_;
    if (d.started || d.stop_requested)
        return;
    d.started = true;
    if (d.cfg.transport == Config::Transport::tcp_listen)
        d.openListener(); // no I/O thread yet; on failure begin() retries
    asio::post(d.io, [&d] { d.begin(); });
    if (d.cfg.run_io_thread) {
        d.guard.emplace(asio::make_work_guard(d.io));
        d.thread = std::thread([&d] {
            d.io_thread_id = std::this_thread::get_id();
            d.io.run();
        });
    }
}

void Client::stop()
{
    Impl& d = *impl_;
    if (!d.stop_requested.exchange(true) && d.started) {
        if (!d.cfg.run_io_thread || d.onIoThread()) {
            d.shutdownOnIo(); // poll mode, or stop() from a callback: we are the I/O thread
        } else {
            asio::post(d.io, [&d] { d.shutdownOnIo(); });
        }
        d.guard.reset(); // run() returns once the cancelled handlers have run
    }
    if (!d.onIoThread() && d.thread.joinable())
        d.thread.join();
}

void Client::poll()
{
    Impl& d = *impl_;
    if (!d.started || d.stop_requested || d.cfg.run_io_thread)
        return;
    if (d.io.stopped())
        d.io.restart();
    d.io.poll();
    if (d.callback_error) {
        std::exception_ptr e;
        std::swap(e, d.callback_error);
        std::rethrow_exception(e);
    }
}

size_t Client::trackCount() const { return impl_->count.load(); }

uint16_t Client::localPort() const { return impl_->local_port.load(); }

} // namespace skycue
