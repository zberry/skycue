#define BOOST_TEST_MODULE skycued_forwarder
#include <boost/test/unit_test.hpp>

#include "forwarder.hpp"

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

constexpr auto kWait = 5s;

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
        cfg.tcp_port = 0;
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
    int keyframes = 0, deltas = 0, datagrams = 0;
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
        BOOST_TEST(static_cast<int>(f.type) != static_cast<int>(FrameType::hello));
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
    BOOST_TEST(fx.clients() == 0u);
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
