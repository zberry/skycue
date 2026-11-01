#define BOOST_TEST_MODULE skycued_engine
#include <boost/test/unit_test.hpp>

#include "config.hpp"
#include "dump978_raw_source.hpp"
#include "engine.hpp"
#include "forwarder.hpp"
#include "line_client.hpp"
#include "observation.hpp"
#include "readsb_json_source.hpp"
#include "recorder.hpp"
#include "replay_source.hpp"

#include <skycue/client.hpp>
#include <skycue/types.hpp>

#include <boost/asio.hpp>

#include <dirent.h>
#include <ftw.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using skycue::AddressType;
using skycue::DropReason;
using skycue::Track;
using skycue::TrackId;
using skycued::Config;
using skycued::Engine;
using skycued::Forwarder;
using skycued::ForwarderConfig;
using skycued::ObservationSource;
using skycued::TrackObservation;
namespace asio = boost::asio;
using asio::ip::tcp;
using steady = std::chrono::steady_clock;

namespace {

constexpr auto kWait = 5s;
const std::string kFixture = std::string(SKYCUE_FIXTURE_DIR) + "/readsb-sample.jsonl";

int removeEntry(const char* p, const struct stat*, int, struct FTW*) { return ::remove(p); }

struct TempDir {
    std::string path;
    TempDir()
    {
        const char* base = std::getenv("TMPDIR");
        std::string tmpl = std::string(base && *base ? base : "/tmp") + "/skycue-engine-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (!::mkdtemp(buf.data()))
            throw std::runtime_error("mkdtemp failed");
        path = buf.data();
    }
    ~TempDir() { ::nftw(path.c_str(), removeEntry, 16, FTW_DEPTH | FTW_PHYS); }
};

std::vector<std::string> filesWithSuffix(const std::string& dir, const std::string& suffix)
{
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d)
        return out;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            out.push_back(dir + "/" + name);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::string readAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t countOf(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
        ++n;
    return n;
}

// Runs an io_context on its own thread; call() runs a function there and
// waits for its result, so the engine is only ever touched on that thread.
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

// Records every skycue::Client callback.
struct Events {
    std::mutex m;
    std::condition_variable cv;
    std::vector<Track> updates;
    std::vector<std::pair<TrackId, DropReason>> drops;
    std::vector<steady::time_point> drop_times;

    void attach(skycue::Client& c)
    {
        c.onTrackUpdate([this](const Track& tr) {
            std::lock_guard<std::mutex> lk(m);
            updates.push_back(tr);
            cv.notify_all();
        });
        c.onTrackDropped([this](TrackId id, DropReason r) {
            std::lock_guard<std::mutex> lk(m);
            drops.emplace_back(id, r);
            drop_times.push_back(steady::now());
            cv.notify_all();
        });
    }

    template <class Pred> bool waitFor(Pred p, std::chrono::milliseconds timeout = kWait)
    {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, timeout, p);
    }

    // Callers hold m.
    std::set<TrackId> ids() const
    {
        std::set<TrackId> s;
        for (const auto& u : updates)
            s.insert(u.id);
        return s;
    }
    bool dropped(TrackId id, DropReason r) const
    {
        return std::any_of(drops.begin(), drops.end(),
                           [&](const std::pair<TrackId, DropReason>& d) { return d.first == id && d.second == r; });
    }
    size_t dropsWith(DropReason r) const
    {
        return static_cast<size_t>(std::count_if(drops.begin(), drops.end(),
                                                 [&](const std::pair<TrackId, DropReason>& d) { return d.second == r; }));
    }
    size_t updatesFor(TrackId id) const
    {
        return static_cast<size_t>(
            std::count_if(updates.begin(), updates.end(), [&](const Track& t) { return t.id == id; }));
    }
};

skycue::Config clientConfig(uint16_t port)
{
    skycue::Config c;
    c.transport = skycue::Config::Transport::tcp;
    c.host = "127.0.0.1";
    c.port = port;
    c.reconnect_interval = 100ms;
    return c;
}

Config baseConfig()
{
    Config c;
    c.receiver_lat = 38.9;
    c.receiver_lon = -77.0;
    c.track_timeout_s = 5;
    return c;
}

// An ObservationSource driven by the test (on the io thread).
struct FakeSource : ObservationSource {
    std::function<void(const TrackObservation&)> on_obs;
    std::function<void(bool)> on_conn;
    bool started = false, stopped = false;

    void onObservation(std::function<void(const TrackObservation&)> h) override { on_obs = std::move(h); }
    void onConnection(std::function<void(bool)> h) override { on_conn = std::move(h); }
    void start() override { started = true; }
    void stop() override { stopped = true; }

    void emit(const TrackObservation& o)
    {
        if (on_obs && !stopped)
            on_obs(o);
    }
    void connection(bool up)
    {
        if (on_conn && !stopped)
            on_conn(up);
    }
};

TrackObservation nearbyObservation(uint32_t address, double lat_offset)
{
    TrackObservation o;
    o.id = TrackId{address, AddressType::icao};
    o.source = skycue::Source::adsb_1090;
    o.received_time = std::chrono::system_clock::now();
    o.position_time = o.received_time;
    o.lat = 38.95 + lat_offset;
    o.lon = -77.0;
    o.alt_baro_ft = 5000;
    o.gs_kt = 200;
    o.track_deg = 90;
    return o;
}

// Engine + Forwarder on an IoThread, torn down on that thread.
struct EngineFx {
    IoThread t;
    std::unique_ptr<Forwarder> fwd;
    std::unique_ptr<skycued::Recorder> received, sent;
    std::unique_ptr<Engine> engine;

    EngineFx(const Config& cfg, std::function<std::unique_ptr<ObservationSource>(asio::io_context&)> make_source,
             std::chrono::milliseconds keyframe_interval = 10s, const std::string& record_dir = "")
    {
        t.call([&] {
            ForwarderConfig fc;
            fc.tcp_port = 0;
            fc.keyframe_interval = keyframe_interval;
            fwd = std::make_unique<Forwarder>(t.io, fc);
            if (!record_dir.empty()) {
                skycued::RecorderConfig rc;
                rc.dir = record_dir;
                rc.csv = false;
                received = std::make_unique<skycued::Recorder>(rc, "received");
                sent = std::make_unique<skycued::Recorder>(rc, "sent");
            }
            engine = std::make_unique<Engine>(t.io, cfg, make_source(t.io), nullptr, *fwd, received.get(), sent.get());
            engine->start();
        });
    }
    ~EngineFx()
    {
        try {
            t.call([this] {
                engine.reset();
                fwd.reset();
                received.reset();
                sent.reset();
            });
        } catch (...) {
        }
        t.join();
    }
    uint16_t port() { return t.call([this] { return fwd->tcpPort(); }); }
    void stop()
    {
        t.call([this] { engine->stop(); });
    }
    skycued::EngineStats stats()
    {
        return t.call([this] { return engine->stats(); });
    }
};

std::function<std::unique_ptr<ObservationSource>(asio::io_context&)> replay(double speed)
{
    return [speed](asio::io_context& io) -> std::unique_ptr<ObservationSource> {
        return std::make_unique<skycued::ReplayObservationSource>(io, kFixture, speed, false);
    };
}

// A listening socket on an ephemeral port, polled without blocking.
struct TestServer {
    asio::io_context io;
    tcp::acceptor acceptor{io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
    TestServer() { acceptor.non_blocking(true); }
    uint16_t port() const { return acceptor.local_endpoint().port(); }

    std::unique_ptr<tcp::socket> accept(std::chrono::milliseconds timeout)
    {
        const auto deadline = steady::now() + timeout;
        while (steady::now() < deadline) {
            auto s = std::make_unique<tcp::socket>(io);
            boost::system::error_code ec;
            acceptor.accept(*s, ec);
            if (!ec)
                return s;
            std::this_thread::sleep_for(5ms);
        }
        return nullptr;
    }
};

const TrackId kTimesOut{0xac5e81, AddressType::icao};   // stops at t = 60 s
const TrackId kTisb{0x2c4a1f, AddressType::non_icao};   // "~2c4a1f"
const TrackId kCrossesOut{0xa8c3e5, AddressType::icao}; // crosses the 150 nm ring outbound near t = 44 s
const TrackId kBlocked{0xa4f2c1, AddressType::icao};

}  // namespace

BOOST_AUTO_TEST_CASE(replay_to_tcp_client_end_to_end)
{
    Config cfg = baseConfig();
    cfg.track_timeout_s = 5;
    EngineFx fx(cfg, replay(20));  // the 120 s fixture takes 6 s
    Events ev;
    skycue::Client client(clientConfig(fx.port()));
    ev.attach(client);
    client.start();

    BOOST_TEST(ev.waitFor([&] { return ev.ids().size() >= 11 && ev.dropped(kTimesOut, DropReason::timeout); }, 12s));
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.ids().size() >= 11u);
        BOOST_TEST(ev.dropped(kTimesOut, DropReason::timeout));
        BOOST_TEST(ev.ids().count(kTisb) == 1u);  // the TIS-B track's id type is non_icao
        BOOST_TEST(ev.ids().count(TrackId{0x2c4a1f, AddressType::icao}) == 0u);
    }
    const auto st = fx.stats();
    BOOST_TEST(st.observations > 0u);
    BOOST_TEST(st.sent_updates > 0u);
    BOOST_TEST(st.readsb_connected);
    BOOST_TEST(!st.dump978_connected);

    fx.stop();  // shutdown drops for every forwarded track
    BOOST_TEST(ev.waitFor([&] { return ev.dropsWith(DropReason::shutdown) >= 1; }));
    client.stop();
}

BOOST_AUTO_TEST_CASE(filter_change_to_drop)
{
    TempDir tmp;
    {
        std::ofstream block(tmp.path + "/block.txt");
        block << "# test block list\na4f2c1\n";
    }
    Config cfg = baseConfig();
    cfg.max_range_nm = 150;
    cfg.allow_list_file = "";  // allow list empty
    cfg.block_list_file = tmp.path + "/block.txt";
    EngineFx fx(cfg, replay(50));  // 2.4 s
    Events ev;
    skycue::Client client(clientConfig(fx.port()));
    ev.attach(client);
    client.start();

    BOOST_TEST(ev.waitFor([&] { return ev.dropped(kCrossesOut, DropReason::filtered); }, 8s));
    std::this_thread::sleep_for(2500ms);  // past the end of the replay
    std::lock_guard<std::mutex> lk(ev.m);
    BOOST_TEST(ev.updatesFor(kBlocked) == 0u);    // blocked: never sent
    BOOST_TEST(ev.updatesFor(kCrossesOut) >= 1u); // sent while inside the ring
    const auto filtered = std::count_if(ev.drops.begin(), ev.drops.end(), [](const std::pair<TrackId, DropReason>& d) {
        return d.first == kCrossesOut && d.second == DropReason::filtered;
    });
    BOOST_TEST(filtered == 1);  // dropped once when it left
}

BOOST_AUTO_TEST_CASE(filtered_track_is_not_resent)
{
    // Once dropped as filtered, the aircraft leaving the ring is never sent
    // again (it stays outside for the rest of the replay).
    Config cfg = baseConfig();
    cfg.max_range_nm = 150;
    EngineFx fx(cfg, replay(50));
    std::mutex m;
    bool updated_after_drop = false, dropped = false;
    skycue::Client client(clientConfig(fx.port()));
    client.onTrackUpdate([&](const Track& t) {
        std::lock_guard<std::mutex> lk(m);
        if (t.id == kCrossesOut && dropped)
            updated_after_drop = true;
    });
    client.onTrackDropped([&](TrackId id, DropReason r) {
        std::lock_guard<std::mutex> lk(m);
        if (id == kCrossesOut && r == DropReason::filtered)
            dropped = true;
    });
    client.start();
    std::this_thread::sleep_for(4s);  // whole replay plus margin
    std::lock_guard<std::mutex> lk(m);
    BOOST_TEST(dropped);
    BOOST_TEST(!updated_after_drop);
}

BOOST_AUTO_TEST_CASE(line_client_reconnects_with_backoff)
{
    TestServer server;
    IoThread t;
    std::mutex m;
    std::vector<bool> conns;
    std::unique_ptr<skycued::LineClient> lc;
    t.call([&] {
        lc = std::make_unique<skycued::LineClient>(t.io, "127.0.0.1", server.port(), "test");
        lc->onConnection([&](bool up) {
            std::lock_guard<std::mutex> lk(m);
            conns.push_back(up);
        });
        lc->start();
    });

    auto first = server.accept(3s);
    BOOST_REQUIRE(first);
    const auto closed_at = steady::now();
    first->close();
    auto second = server.accept(3s);
    BOOST_REQUIRE(second);
    BOOST_TEST(std::chrono::duration<double>(steady::now() - closed_at).count() < 2.0);
    std::this_thread::sleep_for(100ms);
    BOOST_TEST(t.call([&] { return lc->connected(); }));
    {
        std::lock_guard<std::mutex> lk(m);
        BOOST_TEST((conns == std::vector<bool>{true, false, true}));
    }
    t.call([&] {
        lc->stop();
        lc.reset();
    });
}

BOOST_AUTO_TEST_CASE(oversize_line_discarded)
{
    std::string valid;
    {
        std::ifstream in(kFixture);
        std::getline(in, valid);
    }
    TestServer server;
    IoThread t;
    std::mutex m;
    std::condition_variable cv;
    std::vector<TrackObservation> got;
    std::unique_ptr<skycued::ReadsbJsonSource> src;
    t.call([&] {
        src = std::make_unique<skycued::ReadsbJsonSource>(t.io, "127.0.0.1", server.port());
        src->onObservation([&](const TrackObservation& o) {
            std::lock_guard<std::mutex> lk(m);
            got.push_back(o);
            cv.notify_all();
        });
        src->start();
    });
    auto sock = server.accept(3s);
    BOOST_REQUIRE(sock);
    sock->non_blocking(false);
    const std::string payload = std::string(70 * 1024, 'x') + "\n" + valid + "\n";
    asio::write(*sock, asio::buffer(payload));
    {
        std::unique_lock<std::mutex> lk(m);
        BOOST_TEST(cv.wait_for(lk, kWait, [&] { return got.size() == 1; }));
    }
    BOOST_TEST(t.call([&] { return src->linesRejected(); }) == 1u);
    BOOST_TEST(t.call([&] { return src->linesParsed(); }) == 1u);
    BOOST_TEST(got.at(0).id.address == 0xac5e81u);
    t.call([&] {
        src->stop();
        src.reset();
    });
}

BOOST_AUTO_TEST_CASE(dump978_raw_lines_counted)
{
    TestServer server;
    IoThread t;
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> uplinks;
    std::unique_ptr<skycued::Dump978RawSource> src;
    t.call([&] {
        src = std::make_unique<skycued::Dump978RawSource>(t.io, "127.0.0.1", server.port());
        src->onUplink([&](std::chrono::system_clock::time_point, const std::string& hex) {
            std::lock_guard<std::mutex> lk(m);
            uplinks.push_back(hex);
            cv.notify_all();
        });
        src->start();
    });
    auto sock = server.accept(3s);
    BOOST_REQUIRE(sock);
    sock->non_blocking(false);
    const std::string payload = "-00a66ef135445d525a0c0519119021204800;rs=1;rssi=-10.0;\n"
                                "!meta;\n"
                                "+3c5e8a;rs=2;\r\n"
                                "-00a66ef1354;\n";
    asio::write(*sock, asio::buffer(payload));
    {
        std::unique_lock<std::mutex> lk(m);
        BOOST_TEST(cv.wait_for(lk, kWait, [&] { return uplinks.size() == 1; }));
    }
    std::this_thread::sleep_for(100ms);
    BOOST_TEST(uplinks.at(0) == "3c5e8a");
    BOOST_TEST(t.call([&] { return src->downlinkFrames(); }) == 2u);
    BOOST_TEST(t.call([&] { return src->uplinkFrames(); }) == 1u);
    t.call([&] {
        src->stop();
        src.reset();
    });
}

BOOST_AUTO_TEST_CASE(filtered_drop_then_resent_when_eligible)
{
    Config cfg = baseConfig();
    cfg.max_range_nm = 20;
    cfg.min_interval_s = 0;
    FakeSource* fake = nullptr;
    EngineFx fx(cfg, [&](asio::io_context&) -> std::unique_ptr<ObservationSource> {
        auto s = std::make_unique<FakeSource>();
        fake = s.get();
        return s;
    });
    Events ev;
    skycue::Client client(clientConfig(fx.port()));
    ev.attach(client);
    client.start();
    std::this_thread::sleep_for(300ms);
    const TrackId id{0x333333, AddressType::icao};
    fx.t.call([&] {
        fake->connection(true);
        TrackObservation nopos = nearbyObservation(0x444444, 0);  // never positioned: never sent
        nopos.lat.reset();
        nopos.lon.reset();
        nopos.position_time.reset();
        fake->emit(nopos);
        fake->emit(nearbyObservation(id.address, 0));  // 3 nm: sent
    });
    BOOST_TEST(ev.waitFor([&] { return ev.updatesFor(id) == 1; }));
    fx.t.call([&] { fake->emit(nearbyObservation(id.address, 0.6)); });  // 39 nm: dropped
    BOOST_TEST(ev.waitFor([&] { return ev.dropped(id, DropReason::filtered); }));
    fx.t.call([&] { fake->emit(nearbyObservation(id.address, 0.61)); });  // still outside: nothing
    fx.t.call([&] { fake->emit(nearbyObservation(id.address, 0.01)); });  // back inside: sent again
    BOOST_TEST(ev.waitFor([&] { return ev.updatesFor(id) == 2; }));
    std::this_thread::sleep_for(200ms);
    std::lock_guard<std::mutex> lk(ev.m);
    BOOST_TEST(ev.updatesFor(id) == 2u);
    BOOST_TEST(ev.drops.size() == 1u);
    BOOST_TEST(ev.updatesFor(TrackId{0x444444, AddressType::icao}) == 0u);
    BOOST_TEST(!ev.updates.back().keyframe);
}

BOOST_AUTO_TEST_CASE(source_lost_drops_after_timeout)
{
    Config cfg = baseConfig();
    cfg.track_timeout_s = 2;
    FakeSource* fake = nullptr;
    EngineFx fx(cfg, [&](asio::io_context&) -> std::unique_ptr<ObservationSource> {
        auto s = std::make_unique<FakeSource>();
        fake = s.get();
        return s;
    });
    Events ev;
    skycue::Client client(clientConfig(fx.port()));
    ev.attach(client);
    client.start();
    // Let the client connect before anything is sent.
    std::this_thread::sleep_for(300ms);
    auto feed = [&] {
        fx.t.call([&] {
            fake->emit(nearbyObservation(0x111111, 0));
            fake->emit(nearbyObservation(0x222222, 0.01));
        });
    };
    fx.t.call([&] { fake->connection(true); });
    feed();
    BOOST_TEST(ev.waitFor([&] { return ev.ids().size() == 2; }));
    BOOST_TEST(fx.stats().readsb_connected);

    // A short outage: reconnecting within the timeout cancels the drop.
    fx.t.call([&] { fake->connection(false); });
    BOOST_TEST(!fx.stats().readsb_connected);
    std::this_thread::sleep_for(1s);
    fx.t.call([&] { fake->connection(true); });
    for (int i = 0; i < 5; ++i) {
        feed();
        std::this_thread::sleep_for(500ms);
    }
    {
        std::lock_guard<std::mutex> lk(ev.m);
        BOOST_TEST(ev.drops.empty());
    }

    // A long one: every forwarded track is dropped as source_lost.
    const auto lost_at = steady::now();
    fx.t.call([&] { fake->connection(false); });
    BOOST_TEST(ev.waitFor([&] { return ev.drops.size() >= 2; }, 5s));
    std::lock_guard<std::mutex> lk(ev.m);
    BOOST_TEST(ev.drops.size() == 2u);
    BOOST_TEST(ev.dropsWith(DropReason::source_lost) == 2u);
    for (const auto& when : ev.drop_times)
        BOOST_TEST(std::chrono::duration<double>(when - lost_at).count() > 1.4);
}

BOOST_AUTO_TEST_CASE(keyframes_not_recorded_but_updates_are)
{
    TempDir tmp;
    Config cfg = baseConfig();
    cfg.track_timeout_s = 1;
    {
        EngineFx fx(cfg, replay(50), 500ms, tmp.path);
        Events ev;
        skycue::Client client(clientConfig(fx.port()));
        ev.attach(client);
        client.start();
        std::this_thread::sleep_for(3s);  // replay (2.4 s) with several keyframe bursts
        {
            std::lock_guard<std::mutex> lk(ev.m);
            const auto keyframes = std::count_if(ev.updates.begin(), ev.updates.end(),
                                                 [](const Track& tr) { return tr.keyframe; });
            BOOST_TEST(keyframes >= 1);  // keyframes were sent, so "not recorded" means something
        }
        fx.stop();
        BOOST_TEST(ev.waitFor([&] { return ev.dropsWith(DropReason::shutdown) >= 1 || !ev.drops.empty(); }));
        client.stop();
    }
    const auto sent = filesWithSuffix(tmp.path, ".jsonl");
    std::string sent_text, received_text;
    for (const auto& f : sent) {
        if (f.find("/sent-") != std::string::npos)
            sent_text += readAll(f);
        else if (f.find("/received-") != std::string::npos)
            received_text += readAll(f);
    }
    BOOST_TEST(countOf(sent_text, "\"event\":\"update\"") > 0u);
    BOOST_TEST(countOf(sent_text, "\"event\":\"drop\"") > 0u);
    BOOST_TEST(countOf(sent_text, "\"event\":\"keyframe\"") == 0u);
    BOOST_TEST(countOf(sent_text, "\"event\":\"filter_change\"") == 1u);
    BOOST_TEST(countOf(received_text, "\"type\":\"obs\"") > 1000u);
}

BOOST_AUTO_TEST_CASE(replay_reads_a_readsb_capture)
{
    // The fixture is readsb JSON-port output, not a received recording.
    size_t parseable = 0;
    {
        std::ifstream in(kFixture);
        std::string line;
        while (std::getline(in, line))
            parseable += skycued::parseReadsbLine(line) ? 1 : 0;
    }
    asio::io_context io;
    skycued::ReplayObservationSource src(io, kFixture, 0, false);
    std::vector<bool> conns;
    size_t got = 0;
    src.onObservation([&](const TrackObservation&) { ++got; });
    src.onConnection([&](bool up) { conns.push_back(up); });
    src.start();
    io.run();
    BOOST_TEST(parseable > 1000u);
    BOOST_TEST(src.recordCount() == parseable);
    BOOST_TEST(got == parseable);
    BOOST_TEST((conns == std::vector<bool>{true, false}));  // the end counts as lost
}

BOOST_AUTO_TEST_CASE(startup_filter_change_records_list_sizes)
{
    TempDir tmp;
    {
        std::ofstream(tmp.path + "/block.txt") << "a4f2c1\n";
        std::ofstream(tmp.path + "/allow.txt") << "N512RG\nab12f0\n";
    }
    Config cfg = baseConfig();
    cfg.block_list_file = tmp.path + "/block.txt";
    cfg.allow_list_file = tmp.path + "/allow.txt";
    {
        EngineFx fx(
            cfg, [](asio::io_context&) -> std::unique_ptr<ObservationSource> { return std::make_unique<FakeSource>(); },
            10s, tmp.path);
        fx.stop();
    }
    std::string sent_text;
    for (const auto& f : filesWithSuffix(tmp.path, ".jsonl"))
        if (f.find("/sent-") != std::string::npos)
            sent_text += readAll(f);
    BOOST_TEST(countOf(sent_text, "\"event\":\"filter_change\"") == 1u);
    BOOST_TEST(countOf(sent_text, "allow.txt (2 entries)") == 1u);
    BOOST_TEST(countOf(sent_text, "block.txt (1 entries)") == 1u);
}

BOOST_AUTO_TEST_CASE(forwarded_track_dropped_on_first_filtered_observation)
{
    // With large thresholds shouldSend gives no reason for a small move, yet
    // leaving the area must drop the track at once.
    Config cfg = baseConfig();
    cfg.max_range_nm = 20;
    cfg.min_interval_s = 5;
    cfg.max_interval_s = 60;
    FakeSource* fake = nullptr;
    EngineFx fx(cfg, [&](asio::io_context&) -> std::unique_ptr<ObservationSource> {
        auto s = std::make_unique<FakeSource>();
        fake = s.get();
        return s;
    });
    Events ev;
    skycue::Client client(clientConfig(fx.port()));
    ev.attach(client);
    client.start();
    std::this_thread::sleep_for(300ms);
    const TrackId id{0x555555, AddressType::icao};
    fx.t.call([&] {
        fake->connection(true);
        fake->emit(nearbyObservation(id.address, 0.27));  // about 19.2 nm: inside
    });
    BOOST_TEST(ev.waitFor([&] { return ev.updatesFor(id) >= 1; }));
    fx.t.call([&] { fake->emit(nearbyObservation(id.address, 0.30)); });  // about 21 nm: outside, < 5 s later
    BOOST_TEST(ev.waitFor([&] { return ev.dropped(id, DropReason::filtered); }, 1500ms));
    std::lock_guard<std::mutex> lk(ev.m);
    BOOST_TEST(ev.updatesFor(id) == 1u);
}
