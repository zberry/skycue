#define BOOST_TEST_MODULE skycued_forwarder
#include <boost/test/unit_test.hpp>

#include "forwarder.hpp"
#include "test_slow.hpp"

#include <skycue/client.hpp>
#include <skycue/protocol.hpp>
#include <skycue/types.hpp>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace skycue;
using namespace std::chrono_literals;
using skycued::Forwarder;
using skycued::ForwarderConfig;
namespace asio = boost::asio;
using asio::ip::tcp;
using asio::ip::udp;
using steady = std::chrono::steady_clock;

namespace {

const std::chrono::milliseconds kWait = slow(5s);

Track mkTrack(uint32_t address, bool keyframe)
{
    Track t;
    t.id = TrackId{address, AddressType::icao};
    t.source = Source::adsb_1090;
    t.position_time = std::chrono::system_clock::now();
    t.lat_deg = 38.9;
    t.lon_deg = -77.0;
    t.keyframe = keyframe;
    t.alt_baro_ft = 10000;
    t.callsign = "TEST" + std::to_string(address % 1000);
    return t;
}

std::vector<Track> mkTracks(uint32_t n)
{
    std::vector<Track> out;
    for (uint32_t i = 1; i <= n; ++i)
        out.push_back(mkTrack(i, true));
    return out;
}

// Runs an io_context on its own thread; call() runs a function there and
// waits for its result, so the Forwarder is only ever touched on that thread.
struct IoThread {
    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard;
    std::thread thread;

    IoThread()
    {
        guard.emplace(asio::make_work_guard(io));
        thread = std::thread([this] { io.run(); });
    }
    ~IoThread() { join(); }

    void join()
    {
        guard.reset();
        io.stop();
        if (thread.joinable())
            thread.join();
    }

    template <class F> auto call(F f) -> decltype(f())
    {
        using R = decltype(f());
        auto task = std::make_shared<std::packaged_task<R()>>(std::move(f));
        auto fut = task->get_future();
        asio::post(io, [task] { (*task)(); });
        if (fut.wait_for(kWait) != std::future_status::ready)
            throw std::runtime_error("io thread call timed out");
        return fut.get();
    }
};

// A Forwarder on an IoThread. Members are destroyed in reverse order, and the
// destructor resets the Forwarder on its thread before joining it.
struct Fx {
    IoThread t;
    std::unique_ptr<Forwarder> fwd;

    explicit Fx(ForwarderConfig cfg, Forwarder::KeyframeProvider provider = {})
    {
        cfg.tcp_listen_ephemeral = true; // listen, on a port of the OS's choosing
        t.call([&] {
            fwd = std::make_unique<Forwarder>(t.io, cfg);
            if (provider)
                fwd->setKeyframeProvider(provider);
            fwd->start();
        });
    }
    ~Fx()
    {
        try {
            t.call([this] { fwd.reset(); });
        } catch (...) {
        }
        t.join();
    }

    uint16_t port() { return t.call([this] { return fwd->tcpPort(); }); }
    size_t clients() { return t.call([this] { return fwd->tcpClientCount(); }); }
    std::vector<skycued::CueTcpClient> tcpClients() { return t.call([this] { return fwd->tcpClients(); }); }
    std::vector<skycued::CueUdpTarget> udpTargets() { return t.call([this] { return fwd->udpTargets(); }); }
    void update(const Track& tr)
    {
        t.call([&] { fwd->sendUpdate(tr); });
    }
    void stop()
    {
        t.call([this] { fwd->stop(); });
    }
};

// Records every Client callback; tests wait on it with a timeout.
struct Events {
    std::mutex m;
    std::condition_variable cv;
    std::vector<Track> updates;
    std::vector<std::pair<TrackId, DropReason>> drops;
    std::vector<bool> conns;

    void attach(Client& c)
    {
        c.onTrackUpdate([this](const Track& tr) {
            std::lock_guard<std::mutex> lk(m);
            updates.push_back(tr);
            cv.notify_all();
        });
        c.onTrackDropped([this](TrackId id, DropReason r) {
            std::lock_guard<std::mutex> lk(m);
            drops.emplace_back(id, r);
            cv.notify_all();
        });
        c.onConnectionChange([this](bool up) {
            std::lock_guard<std::mutex> lk(m);
            conns.push_back(up);
            cv.notify_all();
        });
    }

    template <class Pred> bool waitFor(Pred p, std::chrono::milliseconds timeout = kWait)
    {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, timeout, p);
    }

    size_t updateCount()
    {
        std::lock_guard<std::mutex> lk(m);
        return updates.size();
    }
};

Config tcpConfig(uint16_t port)
{
    Config c;
    c.transport = Config::Transport::tcp;
    c.host = "127.0.0.1";
    c.port = port;
    c.reconnect_interval = 100ms;
    return c;
}

// A raw TCP connection to the forwarder; reads frames with a timeout.
struct RawTcp {
    asio::io_context io;
    tcp::socket sock{io};
    std::vector<uint8_t> rx;
    uint64_t bytes_read = 0;

    explicit RawTcp(uint16_t port, std::optional<int> rcvbuf = std::nullopt)
    {
        sock.open(tcp::v4());
        if (rcvbuf)
            sock.set_option(asio::socket_base::receive_buffer_size(*rcvbuf));
        sock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
        sock.non_blocking(true);
    }

    std::optional<Frame> next(std::chrono::milliseconds timeout)
    {
        const auto deadline = steady::now() + timeout;
        for (;;) {
            Frame f;
            size_t used = 0;
            const auto st = decodeFrame(rx.data(), rx.size(), f, used);
            if (st == DecodeStatus::ok) {
                rx.erase(rx.begin(), rx.begin() + static_cast<std::ptrdiff_t>(used));
                return f;
            }
            if (st != DecodeStatus::need_more)
                throw std::runtime_error("bad frame on raw tcp");
            uint8_t buf[4096];
            boost::system::error_code ec;
            const size_t n = sock.read_some(asio::buffer(buf), ec);
            if (!ec) {
                rx.insert(rx.end(), buf, buf + n);
                bytes_read += n;
                continue;
            }
            if (ec != asio::error::would_block)
                return std::nullopt;
            if (steady::now() >= deadline)
                return std::nullopt;
            std::this_thread::sleep_for(2ms);
        }
    }
};

// A raw UDP socket on an ephemeral port; returns whole datagrams.
struct RawUdp {
    asio::io_context io;
    udp::socket sock{io, udp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};

    RawUdp() { sock.non_blocking(true); }
    uint16_t port() const { return sock.local_endpoint().port(); }

    std::optional<std::vector<uint8_t>> next(std::chrono::milliseconds timeout)
    {
        const auto deadline = steady::now() + timeout;
        std::vector<uint8_t> buf(65536);
        for (;;) {
            udp::endpoint from;
            boost::system::error_code ec;
            const size_t n = sock.receive_from(asio::buffer(buf), from, 0, ec);
            if (!ec) {
                buf.resize(n);
                return buf;
            }
            if (ec != asio::error::would_block || steady::now() >= deadline)
                return std::nullopt;
            std::this_thread::sleep_for(2ms);
        }
    }
};

uint16_t freeUdpPort()
{
    asio::io_context io;
    udp::socket s(io, udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return s.local_endpoint().port();
}

} // namespace

BOOST_AUTO_TEST_CASE(tcp_client_gets_hello_and_keyframe_on_connect)
{
    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    cfg.heartbeat_interval = 1s;
    cfg.sender_name = "test-sender";
    Fx fx(cfg, [] { return mkTracks(200); });

    // The raw view: Hello first, then 200 keyframe TrackUpdates.
    RawTcp raw(fx.port());
    auto first = raw.next(kWait);
    BOOST_REQUIRE(first);
    Hello h;
    BOOST_REQUIRE(decodeHello(*first, h));
    BOOST_TEST(h.keyframe_interval_ms == 10000u);
    BOOST_TEST(h.heartbeat_interval_ms == 1000u);
    BOOST_TEST(h.sender == "test-sender");
    for (int i = 0; i < 200; ++i) {
        auto f = raw.next(kWait);
        BOOST_REQUIRE(f);
        Track tr;
        BOOST_REQUIRE(decodeTrackUpdate(*f, tr));
        BOOST_TEST(tr.keyframe);
    }

    // A fresh Client has all 200 tracks within 1 s.
    Events ev;
    Client c(tcpConfig(fx.port()));
    ev.attach(c);
    const auto t0 = steady::now();
    c.start();
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() >= 200; }, 1000ms));
    BOOST_TEST(c.trackCount() == 200u);
    BOOST_TEST_MESSAGE("keyframe of 200 tracks in "
                       << std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - t0).count() << " ms");
    BOOST_TEST(fx.clients() == 2u);
}

BOOST_AUTO_TEST_CASE(delta_after_keyframe)
{
    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    Fx fx(cfg, [] { return mkTracks(10); });

    Events ev;
    Client c(tcpConfig(fx.port()));
    ev.attach(c);
    c.start();
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 10; }));

    Track d = mkTrack(3, false);
    d.alt_baro_ft = 12000;
    fx.update(d);
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 11; }));
    std::this_thread::sleep_for(200ms);
    std::lock_guard<std::mutex> lk(ev.m);
    BOOST_TEST(ev.updates.size() == 11u);
    BOOST_TEST((ev.updates.back().id == d.id));
    BOOST_TEST(!ev.updates.back().keyframe);
    BOOST_TEST(*ev.updates.back().alt_baro_ft == 12000);
}

BOOST_AUTO_TEST_CASE(two_clients_independent)
{
    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    Fx fx(cfg, [] { return mkTracks(5); });

    Events ev1, ev2;
    auto c1 = std::make_unique<Client>(tcpConfig(fx.port()));
    Client c2(tcpConfig(fx.port()));
    ev1.attach(*c1);
    ev2.attach(c2);
    c1->start();
    c2.start();
    BOOST_REQUIRE(ev1.waitFor([&] { return ev1.updates.size() == 5; }));
    BOOST_REQUIRE(ev2.waitFor([&] { return ev2.updates.size() == 5; }));
    BOOST_TEST(fx.clients() == 2u);

    fx.update(mkTrack(100, false));
    BOOST_REQUIRE(ev1.waitFor([&] { return ev1.updates.size() == 6; }));
    BOOST_REQUIRE(ev2.waitFor([&] { return ev2.updates.size() == 6; }));

    // Closing one client leaves the other untouched.
    c1.reset();
    const auto deadline = steady::now() + kWait;
    while (fx.clients() != 1 && steady::now() < deadline)
        std::this_thread::sleep_for(10ms);
    BOOST_REQUIRE(fx.clients() == 1u);

    fx.update(mkTrack(101, false));
    BOOST_REQUIRE(ev2.waitFor([&] { return ev2.updates.size() == 7; }));
    BOOST_TEST(c2.trackCount() == 7u);
    std::lock_guard<std::mutex> lk(ev2.m);
    BOOST_TEST(ev2.conns.size() == 1u);
}

BOOST_AUTO_TEST_CASE(slow_client_disconnected_not_others)
{
    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    Fx fx(cfg);

    RawTcp slow(fx.port(), 4096); // never read
    Events ev;
    Client good(tcpConfig(fx.port()));
    ev.attach(good);
    good.start();
    BOOST_REQUIRE(ev.waitFor([&] { return !ev.conns.empty(); }));
    {
        const auto deadline = steady::now() + kWait;
        while (fx.clients() != 2 && steady::now() < deadline)
            std::this_thread::sleep_for(10ms);
    }
    BOOST_REQUIRE(fx.clients() == 2u);

    // Push in batches, each one fully received by the good client before the
    // next, until the slow one is cut off (and at least 2 MiB in total).
    const size_t frame_size = encodeTrackUpdate(mkTrack(1, false)).size();
    constexpr size_t kBatch = 1000;
    size_t sent = 0, pushed_bytes = 0;
    while (pushed_bytes < (64u << 20) && (pushed_bytes < (2u << 20) || fx.clients() == 2)) {
        fx.t.call([&] {
            for (size_t i = 0; i < kBatch; ++i)
                fx.fwd->sendUpdate(mkTrack(static_cast<uint32_t>(1 + i), false));
        });
        sent += kBatch;
        pushed_bytes += kBatch * frame_size;
        BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == sent; }));
    }
    BOOST_TEST_MESSAGE("slow client dropped after " << pushed_bytes / 1024 << " KiB");
    BOOST_TEST(fx.clients() == 1u);
    BOOST_TEST(pushed_bytes >= (2u << 20));

    // The good client is still connected and keeps receiving.
    fx.update(mkTrack(5000, false));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == sent + 1; }));
    std::lock_guard<std::mutex> lk(ev.m);
    BOOST_TEST(ev.conns.size() == 1u);
    BOOST_TEST(ev.conns.front());
}

BOOST_AUTO_TEST_CASE(udp_targets_receive)
{
    RawUdp raw;
    const uint16_t client_port = freeUdpPort();

    ForwarderConfig cfg;
    cfg.keyframe_interval = 300ms;
    cfg.heartbeat_interval = 200ms;
    cfg.udp_targets = {{"127.0.0.1", client_port}, {"127.0.0.1", raw.port()}};

    Config cc;
    cc.transport = Config::Transport::udp;
    cc.host = "127.0.0.1";
    cc.port = client_port;
    Events ev;
    Client c(cc);
    ev.attach(c);
    c.start();
    std::this_thread::sleep_for(50ms); // let the client bind before frames arrive

    Fx fx(cfg, [] { return mkTracks(5); });
    BOOST_REQUIRE(ev.waitFor([&] { return c.trackCount() == 5; }));

    fx.update(mkTrack(0x99, false));
    BOOST_REQUIRE(ev.waitFor([&] {
        for (const auto& tr : ev.updates)
            if (tr.id.address == 0x99)
                return true;
        return false;
    }));

    // Every datagram is exactly one frame of legal size; keyframes arrive.
    int keyframes = 0, deltas = 0, datagrams = 0, hellos = 0;
    const auto deadline = steady::now() + 1s;
    while (steady::now() < deadline) {
        auto d = raw.next(50ms);
        if (!d)
            continue;
        ++datagrams;
        Frame f;
        size_t used = 0;
        BOOST_REQUIRE(decodeFrame(d->data(), d->size(), f, used) == DecodeStatus::ok);
        BOOST_TEST(used == d->size());
        BOOST_TEST(d->size() <= kHeaderSize + kMaxUdpPayload);
        Hello h;
        if (decodeHello(f, h)) {
            ++hellos;
            BOOST_TEST(h.keyframe_interval_ms == 300u);
        }
        Track tr;
        if (decodeTrackUpdate(f, tr)) {
            if (tr.keyframe)
                ++keyframes;
            else if (tr.id.address == 0x99)
                ++deltas;
        }
    }
    BOOST_TEST(datagrams > 0);
    BOOST_TEST(keyframes >= 5);
    BOOST_TEST(deltas == 1);
    BOOST_TEST(hellos >= 2); // one before each keyframe burst
    BOOST_TEST(fx.clients() == 0u);
}

BOOST_AUTO_TEST_CASE(udp_client_learns_keyframe_interval)
{
    const uint16_t client_port = freeUdpPort();
    ForwarderConfig cfg;
    cfg.keyframe_interval = 300ms;
    cfg.heartbeat_interval = 100ms; // keeps the UDP client connected
    cfg.udp_targets = {{"127.0.0.1", client_port}};

    Config cc;
    cc.transport = Config::Transport::udp;
    cc.host = "127.0.0.1";
    cc.port = client_port;
    cc.default_keyframe_interval = 10s; // stale only after 20 s unless a Hello says 300 ms
    Events ev;
    Client c(cc);
    ev.attach(c);
    c.start();
    std::this_thread::sleep_for(50ms);

    // Track A is in the first two keyframe bursts only.
    auto calls = std::make_shared<std::atomic<int>>(0);
    Fx fx(cfg, [calls] {
        if (++*calls <= 2)
            return std::vector<Track>{mkTrack(0xA, true)};
        return std::vector<Track>{};
    });
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 2; }));
    const auto t0 = steady::now();
    BOOST_REQUIRE(ev.waitFor([&] { return !ev.drops.empty(); }, 1500ms));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - t0).count();
    BOOST_TEST_MESSAGE("stale drop " << ms << " ms after the last keyframe");
    std::lock_guard<std::mutex> lk(ev.m);
    BOOST_TEST(ev.drops.front().first.address == 0xAu);
    BOOST_TEST(static_cast<int>(ev.drops.front().second) == static_cast<int>(DropReason::stale));
    BOOST_TEST(ev.conns.size() == 1u); // still connected: not source_lost
}

BOOST_AUTO_TEST_CASE(heartbeat_when_idle)
{
    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    cfg.heartbeat_interval = 400ms;
    Fx fx(cfg);

    RawTcp raw(fx.port());
    auto hello = raw.next(kWait);
    BOOST_REQUIRE(hello);
    BOOST_TEST(static_cast<int>(hello->type) == 1);
    const auto t0 = steady::now();
    auto f = raw.next(cfg.heartbeat_interval * 3 / 2);
    BOOST_REQUIRE(f);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - t0);
    BOOST_TEST(static_cast<int>(f->type) == 4);
    Heartbeat hb;
    BOOST_TEST(decodeHeartbeat(*f, hb));
    BOOST_TEST(elapsed.count() <= (cfg.heartbeat_interval * 3 / 2).count());
    BOOST_TEST_MESSAGE("first heartbeat after " << elapsed.count() << " ms");
}

BOOST_AUTO_TEST_CASE(heartbeat_suppressed_while_traffic_flows)
{
    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    cfg.heartbeat_interval = 300ms;
    Fx fx(cfg);

    RawTcp raw(fx.port());
    auto hello = raw.next(kWait);
    BOOST_REQUIRE(hello);

    // An update every 50 ms for 1.2 s: no heartbeat in between.
    int heartbeats = 0, updates = 0;
    const auto until = steady::now() + 1200ms;
    while (steady::now() < until) {
        fx.update(mkTrack(7, false));
        const auto slice = steady::now() + 50ms;
        while (steady::now() < slice) {
            auto f = raw.next(5ms);
            if (!f)
                continue;
            if (f->type == FrameType::heartbeat)
                ++heartbeats;
            else if (f->type == FrameType::track_update)
                ++updates;
        }
    }
    BOOST_TEST(updates >= 10);
    BOOST_TEST(heartbeats == 0);

    // Idle again: a heartbeat follows.
    std::optional<Frame> f;
    do
        f = raw.next(cfg.heartbeat_interval * 3 / 2);
    while (f && f->type == FrameType::track_update);
    BOOST_REQUIRE(f);
    BOOST_TEST(static_cast<int>(f->type) == 4);
}

BOOST_AUTO_TEST_CASE(stop_sends_shutdown_drops)
{
    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    RawUdp raw;
    cfg.udp_targets = {{"127.0.0.1", raw.port()}};
    Fx fx(cfg, [] { return mkTracks(3); });

    Events ev;
    Client c(tcpConfig(fx.port()));
    ev.attach(c);
    c.start();
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 3; }));

    fx.stop();
    BOOST_REQUIRE(ev.waitFor([&] { return ev.drops.size() == 3; }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        for (const auto& d : ev.drops)
            BOOST_TEST(static_cast<int>(d.second) == static_cast<int>(DropReason::shutdown));
    }
    BOOST_TEST(c.trackCount() == 0u);
    BOOST_REQUIRE(ev.waitFor([&] { return ev.conns.size() == 2; })); // the server closed the connection
    BOOST_TEST(fx.clients() == 0u);

    // The UDP target got the three shutdown drops too.
    int udp_drops = 0;
    const auto deadline = steady::now() + 1s;
    while (udp_drops < 3 && steady::now() < deadline) {
        auto d = raw.next(50ms);
        if (!d)
            continue;
        Frame f;
        size_t used = 0;
        if (decodeFrame(d->data(), d->size(), f, used) != DecodeStatus::ok)
            continue;
        TrackDrop td;
        if (decodeTrackDrop(f, td) && td.reason == DropReason::shutdown)
            ++udp_drops;
    }
    BOOST_TEST(udp_drops == 3);

    // Further sends are ignored; stop is idempotent.
    fx.update(mkTrack(1, false));
    fx.stop();
}

BOOST_AUTO_TEST_CASE(start_throws_when_port_in_use)
{
    asio::io_context io;
    tcp::acceptor taken(io, tcp::endpoint(tcp::v4(), 0));
    ForwarderConfig cfg;
    cfg.tcp_port = taken.local_endpoint().port();
    Forwarder f(io, cfg);
    BOOST_CHECK_THROW(f.start(), std::runtime_error);
}

namespace {

// A Client that listens for the Forwarder to connect to it.
Config listenConfig(uint16_t port = 0)
{
    Config c;
    c.transport = Config::Transport::tcp_listen;
    c.host = "127.0.0.1";
    c.port = port;
    return c;
}

// Records the type of every frame a Client sees, under ev.m.
void recordFrames(Client& c, Events& ev, std::vector<FrameType>& types)
{
    c.onFrame([&ev, &types](const Frame& f) {
        std::lock_guard<std::mutex> lk(ev.m);
        types.push_back(f.type);
        ev.cv.notify_all();
    });
}

template <class Pred> bool waitUntil(Pred p, std::chrono::milliseconds timeout = kWait)
{
    const auto deadline = steady::now() + timeout;
    while (!p()) {
        if (steady::now() >= deadline)
            return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

} // namespace

BOOST_AUTO_TEST_CASE(tcp_connect_target_receives_hello_keyframe_and_deltas)
{
    Events ev;
    std::vector<FrameType> types;
    Client target(listenConfig());
    ev.attach(target);
    recordFrames(target, ev, types);
    target.start();
    BOOST_REQUIRE(target.localPort() != 0);

    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    cfg.tcp_connect_targets = {{"127.0.0.1", target.localPort()}};
    Fx fx(cfg, [] { return mkTracks(200); });

    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 200; }));
    BOOST_TEST(target.trackCount() == 200u);
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_REQUIRE(types.size() == 201u);
        BOOST_TEST(static_cast<int>(types.front()) == static_cast<int>(FrameType::hello));
        for (const auto& tr : ev.updates)
            BOOST_TEST(tr.keyframe);
        BOOST_TEST(ev.conns.size() == 1u);
    }
    BOOST_TEST(fx.clients() == 1u); // the outbound connection counts as a client

    Track d = mkTrack(7, false);
    d.alt_baro_ft = 12000;
    fx.update(d);
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 201; }));
    std::lock_guard<std::mutex> lk(ev.m);
    BOOST_TEST((ev.updates.back().id == d.id));
    BOOST_TEST(!ev.updates.back().keyframe);
    BOOST_TEST(*ev.updates.back().alt_baro_ft == 12000);
}

BOOST_AUTO_TEST_CASE(tcp_connect_reconnects_after_target_restart)
{
    // The target is not up yet: the Forwarder keeps dialling.
    uint16_t port = 0;
    {
        asio::io_context io;
        tcp::acceptor a(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        port = a.local_endpoint().port();
    }
    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    cfg.tcp_connect_targets = {{"127.0.0.1", port}};
    Fx fx(cfg, [] { return mkTracks(3); });
    std::this_thread::sleep_for(700ms); // at least one refused attempt
    BOOST_TEST(fx.clients() == 0u);

    Events ev1;
    std::vector<FrameType> types1;
    auto first = std::make_unique<Client>(listenConfig(port));
    ev1.attach(*first);
    recordFrames(*first, ev1, types1);
    first->start();
    BOOST_REQUIRE(first->localPort() == port);
    BOOST_REQUIRE(ev1.waitFor([&] { return ev1.updates.size() == 3; }));
    BOOST_REQUIRE(waitUntil([&] { return fx.clients() == 1; }));

    // Restart the target on the same port: Hello and keyframe again.
    first.reset();
    BOOST_REQUIRE(waitUntil([&] { return fx.clients() == 0; }));
    Events ev2;
    std::vector<FrameType> types2;
    Client second(listenConfig(port));
    ev2.attach(second);
    recordFrames(second, ev2, types2);
    second.start();
    BOOST_REQUIRE(second.localPort() == port);
    BOOST_REQUIRE(ev2.waitFor([&] { return ev2.updates.size() == 3; }));
    {
        std::lock_guard<std::mutex> lk(ev2.m);
        BOOST_REQUIRE(!types2.empty());
        BOOST_TEST(static_cast<int>(types2.front()) == static_cast<int>(FrameType::hello));
    }
    BOOST_REQUIRE(waitUntil([&] { return fx.clients() == 1; }));
    fx.update(mkTrack(9, false));
    BOOST_REQUIRE(ev2.waitFor([&] { return ev2.updates.size() == 4; }));
}

BOOST_AUTO_TEST_CASE(tcp_port_zero_disables_listener)
{
    Events ev;
    Client target(listenConfig());
    ev.attach(target);
    target.start();
    BOOST_REQUIRE(target.localPort() != 0);

    IoThread t;
    std::unique_ptr<Forwarder> fwd;
    ForwarderConfig cfg;
    cfg.tcp_port = 0;
    cfg.tcp_connect_targets = {{"127.0.0.1", target.localPort()}};
    t.call([&] {
        fwd = std::make_unique<Forwarder>(t.io, cfg);
        fwd->setKeyframeProvider([] { return mkTracks(2); });
        fwd->start(); // no listener: nothing to bind, nothing to throw
    });
    BOOST_TEST(t.call([&] { return fwd->tcpPort(); }) == 0u);

    // Connecting to the configured port fails: nothing listens there.
    {
        asio::io_context io;
        tcp::socket s(io);
        boost::system::error_code ec;
        s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), cfg.tcp_port), ec);
        BOOST_TEST(static_cast<bool>(ec));
    }

    // Pure outbound still works.
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 2; }));
    BOOST_TEST(t.call([&] { return fwd->tcpClientCount(); }) == 1u);
    t.call([&] { fwd.reset(); });
    t.join();
}

BOOST_AUTO_TEST_CASE(tcp_client_bookkeeping)
{
    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    cfg.heartbeat_interval = 10s;
    Fx fx(cfg, [] { return mkTracks(2); });

    auto raw = std::make_unique<RawTcp>(fx.port());
    const std::string local_port = std::to_string(raw->sock.local_endpoint().port());
    BOOST_REQUIRE(waitUntil([&] { return fx.tcpClients().size() == 1; }));
    {
        const auto c = fx.tcpClients().at(0);
        BOOST_TEST(!c.dialled);
        const std::string suffix = ":" + local_port;
        BOOST_REQUIRE(c.endpoint.size() > suffix.size());
        BOOST_TEST(c.endpoint.compare(c.endpoint.size() - suffix.size(), suffix.size(), suffix) == 0);
        const auto age = std::chrono::system_clock::now() - c.connected_since;
        BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(age).count() >= 0);
        BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(age).count() < 5000);
    }

    for (uint32_t i = 0; i < 3; ++i)
        fx.update(mkTrack(0x100 + i, false));
    // Hello + 2 keyframes + 3 updates: bytes_sent catches up with what arrived.
    size_t frames = 0;
    BOOST_REQUIRE(waitUntil([&] {
        while (raw->next(10ms))
            ++frames;
        const auto clients = fx.tcpClients();
        return frames == 6 && clients.size() == 1 && clients[0].bytes_sent == raw->bytes_read;
    }));
    BOOST_TEST(raw->bytes_read > 0u);

    raw.reset();
    BOOST_REQUIRE(waitUntil([&] { return fx.tcpClients().empty(); }));
}

BOOST_AUTO_TEST_CASE(endpoint_text_brackets_ipv6)
{
    BOOST_TEST(skycued::endpointText("127.0.0.1", 30500) == "127.0.0.1:30500");
    BOOST_TEST(skycued::endpointText("::1", 30500) == "[::1]:30500");
}

BOOST_AUTO_TEST_CASE(udp_target_datagram_count)
{
    RawUdp raw;
    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    cfg.heartbeat_interval = 10s;
    cfg.udp_targets = {{"127.0.0.1", raw.port()}};
    Fx fx(cfg);

    for (uint32_t i = 0; i < 3; ++i)
        fx.update(mkTrack(0x200 + i, false));
    // The start() Hello + 3 updates.
    uint64_t received = 0;
    BOOST_REQUIRE(waitUntil([&] {
        while (raw.next(10ms))
            ++received;
        const auto targets = fx.udpTargets();
        return received >= 4 && targets.size() == 1 && targets[0].datagrams_sent == received;
    }));
    const auto targets = fx.udpTargets();
    BOOST_TEST(targets.at(0).endpoint == "127.0.0.1:" + std::to_string(raw.port()));
    BOOST_TEST(received == 4u);
}

BOOST_AUTO_TEST_CASE(dialled_target_marked)
{
    Events ev;
    Client target(listenConfig());
    ev.attach(target);
    target.start();
    BOOST_REQUIRE(target.localPort() != 0);

    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    cfg.tcp_connect_targets = {{"127.0.0.1", target.localPort()}};
    Fx fx(cfg, [] { return mkTracks(3); });

    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 3; }));
    const auto clients = fx.tcpClients();
    BOOST_REQUIRE(clients.size() == 1u);
    BOOST_TEST(clients[0].dialled);
    BOOST_TEST(clients[0].endpoint == "127.0.0.1:" + std::to_string(target.localPort()));
    BOOST_REQUIRE(waitUntil([&] { return fx.tcpClients().at(0).bytes_sent > 0; }));
}

namespace {

uint16_t freeTcpPort()
{
    asio::io_context io;
    tcp::acceptor a(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return a.local_endpoint().port();
}

skycued::ForwarderDestination destination(const std::string& id, skycue::net::Transport transport,
                                          const std::string& host, uint16_t port)
{
    skycue::net::Endpoint e;
    e.transport = transport;
    e.port = port;
    if (transport == skycue::net::Transport::tcp_listen)
        e.bind = host;
    else
        e.host = host;
    return skycued::ForwarderDestination{id, e, false};
}

} // namespace

BOOST_AUTO_TEST_CASE(forwarder_slow_client_dropped_and_regreeted)
{
    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    Fx fx(cfg, [] { return mkTracks(3); });

    RawTcp slow(fx.port(), 4096); // never read
    BOOST_REQUIRE(waitUntil([&] { return fx.clients() == 1; }));

    size_t pushed_bytes = 0;
    const size_t frame_size = encodeTrackUpdate(mkTrack(1, false)).size();
    while (pushed_bytes < (64u << 20) && fx.clients() == 1) {
        fx.t.call([&] {
            for (uint32_t i = 0; i < 1000; ++i)
                fx.fwd->sendUpdate(mkTrack(1 + i, false));
        });
        pushed_bytes += 1000 * frame_size;
        std::this_thread::sleep_for(5ms);
    }
    BOOST_REQUIRE(waitUntil([&] { return fx.clients() == 0; }));

    // The same address connecting again is greeted like any new client.
    RawTcp again(fx.port());
    auto first = again.next(kWait);
    BOOST_REQUIRE(first);
    BOOST_TEST(static_cast<int>(first->type) == static_cast<int>(FrameType::hello));
    for (int i = 0; i < 3; ++i) {
        auto f = again.next(kWait);
        BOOST_REQUIRE(f);
        Track tr;
        BOOST_REQUIRE(decodeTrackUpdate(*f, tr));
        BOOST_TEST(tr.keyframe);
    }
    BOOST_TEST(fx.clients() == 1u);
}

BOOST_AUTO_TEST_CASE(forwarder_dialled_target_late_start)
{
    const uint16_t port = freeTcpPort(); // nothing listens yet
    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    cfg.tcp_connect_targets = {{"127.0.0.1", port}};
    Fx fx(cfg, [] { return mkTracks(3); });
    std::this_thread::sleep_for(1500ms); // several refused attempts, backoff growing
    BOOST_TEST(fx.clients() == 0u);

    Events ev;
    std::vector<FrameType> types;
    Client target(listenConfig(port));
    ev.attach(target);
    recordFrames(target, ev, types);
    target.start();
    BOOST_REQUIRE(target.localPort() == port);

    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 3; }));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_REQUIRE(!types.empty());
        BOOST_TEST(static_cast<int>(types.front()) == static_cast<int>(FrameType::hello));
    }
    BOOST_REQUIRE(waitUntil([&] { return fx.clients() == 1; }));
    BOOST_TEST(fx.tcpClients().at(0).dialled);
}

BOOST_AUTO_TEST_CASE(forwarder_add_and_remove_destination_at_runtime)
{
    ForwarderConfig cfg;
    cfg.keyframe_interval = 10s;
    cfg.heartbeat_interval = 10s;
    Fx fx(cfg, [] { return mkTracks(2); });
    BOOST_TEST(fx.t.call([&] { return fx.fwd->destinations().size(); }) == 1u);
    BOOST_TEST(fx.t.call([&] { return fx.fwd->destinations().at(0).id; }) == "cue-tcp");
    BOOST_TEST(fx.t.call([&] { return fx.fwd->destinations().at(0).builtin; }));

    // A listener added while running: a client gets Hello and the keyframe.
    const uint16_t lport = freeTcpPort();
    fx.t.call([&] {
        fx.fwd->addDestination(destination("extra-listen", skycue::net::Transport::tcp_listen, "127.0.0.1", lport));
    });
    RawTcp raw(lport);
    auto hello = raw.next(kWait);
    BOOST_REQUIRE(hello);
    BOOST_TEST(static_cast<int>(hello->type) == static_cast<int>(FrameType::hello));
    for (int i = 0; i < 2; ++i)
        BOOST_REQUIRE(raw.next(kWait));
    BOOST_REQUIRE(waitUntil([&] { return fx.clients() == 1; }));

    // A UDP target and a dialled target added while running.
    RawUdp udpRaw;
    fx.t.call([&] {
        fx.fwd->addDestination(destination("extra-udp", skycue::net::Transport::udp, "127.0.0.1", udpRaw.port()));
    });
    auto uh = udpRaw.next(kWait);
    BOOST_REQUIRE(uh);
    Frame uf;
    size_t used = 0;
    BOOST_REQUIRE(decodeFrame(uh->data(), uh->size(), uf, used) == DecodeStatus::ok);
    BOOST_TEST(static_cast<int>(uf.type) == static_cast<int>(FrameType::hello));
    BOOST_TEST(fx.udpTargets().size() == 1u);

    Events ev;
    Client target(listenConfig());
    ev.attach(target);
    target.start();
    fx.t.call([&] {
        fx.fwd->addDestination(
            destination("extra-dial", skycue::net::Transport::tcp_connect, "127.0.0.1", target.localPort()));
    });
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 2; }));
    BOOST_REQUIRE(waitUntil([&] { return fx.clients() == 2; }));
    BOOST_TEST(fx.t.call([&] { return fx.fwd->destinations().size(); }) == 4u);
    BOOST_CHECK_THROW(fx.t.call([&] {
        fx.fwd->addDestination(destination("extra-udp", skycue::net::Transport::udp, "127.0.0.1", 9));
    }), std::invalid_argument);

    // Removal closes that destination's peer and sends no drop.
    fx.t.call([&] { fx.fwd->removeDestination("extra-listen"); });
    bool saw_drop = false;
    for (;;) {
        auto f = raw.next(kWait);
        if (!f)
            break; // end of stream
        saw_drop = saw_drop || f->type == FrameType::track_drop;
    }
    BOOST_TEST(!saw_drop);
    BOOST_REQUIRE(waitUntil([&] { return fx.clients() == 1; }));
    fx.t.call([&] { fx.fwd->removeDestination("extra-udp"); });
    BOOST_TEST(fx.udpTargets().empty());
    fx.t.call([&] { fx.fwd->removeDestination("no-such-id"); }); // ignored
    BOOST_TEST(fx.t.call([&] { return fx.fwd->destinations().size(); }) == 2u);

    // The remaining destinations keep working.
    fx.update(mkTrack(9, false));
    BOOST_REQUIRE(ev.waitFor([&] { return ev.updates.size() == 3; }));
}
