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

#include "engine_test_support.hpp"

using namespace enginetest;

namespace {

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

// ---- several inputs (spec §4.4, §4.5) ----

namespace {

// Two readsb-type fake inputs, "a" and "b".
std::vector<InputDef> twoInputs() { return {fakeDef("a"), fakeDef("b")}; }

TrackObservation from(const std::string& input, uint32_t address, double lat_offset)
{
    TrackObservation o = nearbyObservation(address, lat_offset);
    o.input_id = input;
    return o;
}

// A client on the fixture's port, started and given time to connect.
struct Watch {
    Events ev;
    skycue::Client client;
    explicit Watch(EngineFx& fx) : client(clientConfig(fx.port()))
    {
        ev.attach(client);
        client.start();
        std::this_thread::sleep_for(300ms);
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(two_inputs_one_lost_tracks_persist_until_timeout)
{
    Config cfg = baseConfig();
    cfg.track_timeout_s = 2;
    std::map<std::string, FakeInput*> in;
    EngineFx fx(cfg, fakeInputs(twoInputs(), in));
    Watch w(fx);
    const TrackId ta{0x111111, AddressType::icao}, tb{0x222222, AddressType::icao};
    fx.t.call([&] {
        in["a"]->connection(true);
        in["b"]->connection(true);
        in["a"]->emit(from("a", ta.address, 0));
        in["b"]->emit(from("b", tb.address, 0.01));
    });
    BOOST_TEST(w.ev.waitFor([&] { return w.ev.ids().size() == 2; }));

    const auto lost_at = steady::now();
    fx.t.call([&] { in["a"]->connection(false); });
    for (int i = 0; i < 8; ++i) {  // b keeps its track alive for 4 s
        fx.t.call([&] { in["b"]->emit(from("b", tb.address, 0.01)); });
        std::this_thread::sleep_for(500ms);
    }
    std::lock_guard<std::mutex> lk(w.ev.m);
    BOOST_TEST(w.ev.dropped(ta, DropReason::timeout));
    BOOST_TEST(w.ev.drops.size() == 1u);
    BOOST_TEST(w.ev.dropsWith(DropReason::source_lost) == 0u);
    BOOST_TEST(std::chrono::duration<double>(w.ev.drop_times.at(0) - lost_at).count() > 1.4);
}

BOOST_AUTO_TEST_CASE(source_lost_only_when_all_track_inputs_down)
{
    Config cfg = baseConfig();
    cfg.track_timeout_s = 2;
    std::map<std::string, FakeInput*> in;
    EngineFx fx(cfg, fakeInputs(twoInputs(), in));
    Watch w(fx);
    auto feed = [&] {
        fx.t.call([&] {
            in["a"]->emit(from("a", 0x111111, 0));
            in["b"]->emit(from("b", 0x222222, 0.01));
        });
    };
    fx.t.call([&] {
        in["a"]->connection(true);
        in["b"]->connection(true);
    });
    feed();
    BOOST_TEST(w.ev.waitFor([&] { return w.ev.ids().size() == 2; }));

    // Both down, one back within the timeout: nothing is dropped.
    fx.t.call([&] {
        in["a"]->connection(false);
        in["b"]->connection(false);
    });
    std::this_thread::sleep_for(1s);
    fx.t.call([&] { in["b"]->connection(true); });
    for (int i = 0; i < 5; ++i) {
        feed();
        std::this_thread::sleep_for(500ms);
    }
    {
        std::lock_guard<std::mutex> lk(w.ev.m);
        BOOST_TEST(w.ev.drops.empty());
    }

    // Both down for longer: every forwarded track goes as source_lost.
    const auto lost_at = steady::now();
    fx.t.call([&] { in["b"]->connection(false); });
    BOOST_TEST(w.ev.waitFor([&] { return w.ev.drops.size() >= 2; }, 5s));
    std::lock_guard<std::mutex> lk(w.ev.m);
    BOOST_TEST(w.ev.drops.size() == 2u);
    BOOST_TEST(w.ev.dropsWith(DropReason::source_lost) == 2u);
    for (const auto& when : w.ev.drop_times)
        BOOST_TEST(std::chrono::duration<double>(when - lost_at).count() > 1.4);
}

BOOST_AUTO_TEST_CASE(bound_but_silent_input_never_source_lost)
{
    Config cfg = baseConfig();
    cfg.track_timeout_s = 1;
    std::map<std::string, FakeInput*> in;
    EngineFx fx(cfg, fakeInputs({fakeDef("udp1", InputType::pingstation_json)}, in));
    Watch w(fx);
    const TrackId id{0x333333, AddressType::icao};
    fx.t.call([&] {
        in["udp1"]->connection(true);  // bound; the device then goes quiet
        in["udp1"]->emit(from("udp1", id.address, 0));
    });
    BOOST_TEST(w.ev.waitFor([&] { return w.ev.dropped(id, DropReason::timeout); }, 4s));
    std::this_thread::sleep_for(1500ms);
    BOOST_TEST(fx.stats().drops_source_lost == 0u);
    BOOST_TEST(fx.stats().drops_timeout == 1u);
}

BOOST_AUTO_TEST_CASE(dump978_down_does_not_count)
{
    Config cfg = baseConfig();
    cfg.track_timeout_s = 2;
    std::map<std::string, FakeInput*> in;
    EngineFx fx(cfg, fakeInputs({fakeDef("readsb"), fakeDef("dump978", InputType::dump978)}, in));
    Watch w(fx);
    const TrackId id{0x444444, AddressType::icao};
    auto feed = [&] { fx.t.call([&] { in["readsb"]->emit(from("readsb", id.address, 0)); }); };
    fx.t.call([&] {
        in["readsb"]->connection(true);
        in["dump978"]->connection(true);
    });
    feed();
    BOOST_TEST(w.ev.waitFor([&] { return w.ev.updatesFor(id) == 1; }));
    BOOST_TEST(fx.stats().readsb_connected);
    BOOST_TEST(fx.stats().dump978_connected);

    // dump978 goes, readsb stays: no source_lost.
    fx.t.call([&] { in["dump978"]->connection(false); });
    BOOST_TEST(!fx.stats().dump978_connected);
    for (int i = 0; i < 6; ++i) {
        feed();
        std::this_thread::sleep_for(500ms);
    }
    {
        std::lock_guard<std::mutex> lk(w.ev.m);
        BOOST_TEST(w.ev.drops.empty());
    }

    // readsb goes while dump978 is back: dump978 does not keep the tracks.
    fx.t.call([&] {
        in["dump978"]->connection(true);
        in["readsb"]->connection(false);
    });
    BOOST_TEST(w.ev.waitFor([&] { return w.ev.dropped(id, DropReason::source_lost); }, 5s));
}

BOOST_AUTO_TEST_CASE(uplink_duplicate_from_other_input_not_decoded)
{
    TempDir tmp;
    Config cfg = baseConfig();
    std::map<std::string, FakeInput*> in;
    {
        EngineFx fx(cfg,
                    fakeInputs({fakeDef("u1", InputType::dump978), fakeDef("u2", InputType::pingstation_json)}, in),
                    10s, tmp.path);
        const auto t0 = std::chrono::system_clock::now();
        fx.t.call([&] {
            in["u1"]->uplink(t0, "00aa11");
            in["u2"]->uplink(t0 + 10ms, "00aa11");
        });
        const auto s = fx.stats();
        BOOST_TEST(s.fisb_uplinks == 1u);
        BOOST_TEST(s.fisb_uplink_duplicates == 1u);
        fx.stop();
    }
    BOOST_TEST(countOf(streamText(tmp.path, "received"), "\"type\":\"uat_uplink\"") == 1u);
}

BOOST_AUTO_TEST_CASE(uplink_repeat_same_input_decoded)
{
    Config cfg = baseConfig();
    std::map<std::string, FakeInput*> in;
    EngineFx fx(cfg, fakeInputs({fakeDef("u1", InputType::dump978), fakeDef("u2", InputType::dump978)}, in));
    const auto t0 = std::chrono::system_clock::now();
    fx.t.call([&] {
        in["u1"]->uplink(t0, "00aa11");
        in["u1"]->uplink(t0 + 10ms, "00aa11");
    });
    const auto s = fx.stats();
    BOOST_TEST(s.fisb_uplinks == 2u);
    BOOST_TEST(s.fisb_uplink_duplicates == 0u);
}

BOOST_AUTO_TEST_CASE(dedupe_other_input_within_window)
{
    skycued::UplinkDedupe d;
    const auto t0 = steady::now();
    BOOST_TEST(!d.duplicate("a", t0, "abcd"));
    BOOST_TEST(d.duplicate("b", t0 + 1999ms, "abcd"));
    BOOST_TEST(!d.duplicate("b", t0 + 1999ms, "abce"));  // another hex
}

BOOST_AUTO_TEST_CASE(dedupe_after_window_not_duplicate)
{
    skycued::UplinkDedupe d;
    const auto t0 = steady::now();
    BOOST_TEST(!d.duplicate("a", t0, "abcd"));
    BOOST_TEST(!d.duplicate("b", t0 + 2001ms, "abcd"));
}

BOOST_AUTO_TEST_CASE(dedupe_same_input_not_duplicate)
{
    skycued::UplinkDedupe d;
    const auto t0 = steady::now();
    BOOST_TEST(!d.duplicate("a", t0, "abcd"));
    BOOST_TEST(!d.duplicate("a", t0 + 500ms, "abcd"));
    BOOST_TEST(!d.duplicate("a", t0 + 1000ms, "abcd"));
}

BOOST_AUTO_TEST_CASE(two_inputs_same_aircraft_send_rate_bounded)
{
    // a asserts on_ground, squawk and callsign; b carries positions only. b
    // must not make those fields flap and so bypass MIN_INTERVAL_S.
    Config cfg = baseConfig();
    cfg.min_interval_s = 1;
    std::map<std::string, FakeInput*> in;
    EngineFx fx(cfg, fakeInputs(twoInputs(), in));
    Watch w(fx);
    const TrackId id{0x555555, AddressType::icao};
    fx.t.call([&] {
        in["a"]->connection(true);
        in["b"]->connection(true);
    });
    auto lastInput = [&] {
        for (const auto& t : fx.engine->snapshot().tracks)
            if (t.track.id == id)
                return t.input;
        return std::string();
    };
    std::string after_a, after_b;
    const auto end = steady::now() + 3s;
    for (int i = 0; steady::now() < end; ++i) {
        const double offset = 0.0001 * i;
        if (i % 2 == 0) {
            TrackObservation o = from("a", id.address, offset);
            o.on_ground = false;
            o.squawk = "1200";
            o.callsign = "N512RG";
            after_a = fx.t.call([&] {
                in["a"]->emit(o);
                return lastInput();
            });
        } else {
            after_b = fx.t.call([&] {
                in["b"]->emit(from("b", id.address, offset));
                return lastInput();
            });
        }
        std::this_thread::sleep_for(50ms);
    }
    std::this_thread::sleep_for(200ms);
    BOOST_TEST(after_a == "a");
    BOOST_TEST(after_b == "b");
    std::lock_guard<std::mutex> lk(w.ev.m);
    BOOST_TEST(w.ev.updatesFor(id) >= 1u);
    BOOST_TEST(w.ev.updatesFor(id) <= 5u);  // the first plus about one a second
}

BOOST_AUTO_TEST_CASE(lagging_observation_times_still_resend_at_max_interval)
{
    // Observations stamped 0.5 s behind the data clock do not move it, but
    // MAX_INTERVAL_S resends must still happen on time.
    Config cfg = baseConfig();
    cfg.min_interval_s = 0.2;
    cfg.max_interval_s = 1;
    FakeSource* fake = nullptr;
    EngineFx fx(cfg, fakeSource(fake));
    Watch w(fx);
    const TrackId id{0x666666, AddressType::icao};
    fx.t.call([&] {
        fake->connection(true);
        fake->emit(nearbyObservation(id.address, 0));  // sets the data clock
    });
    BOOST_TEST(w.ev.waitFor([&] { return w.ev.updatesFor(id) == 1; }));
    const auto end = steady::now() + 3500ms;
    while (steady::now() < end) {
        TrackObservation o = nearbyObservation(id.address, 0);  // same state: only max_interval sends
        o.received_time -= 500ms;
        o.position_time = o.received_time;
        fx.t.call([&] { fake->emit(o); });
        std::this_thread::sleep_for(100ms);
    }
    std::lock_guard<std::mutex> lk(w.ev.m);
    BOOST_TEST(w.ev.updatesFor(id) >= 3u);  // the first plus a resend about every second
    BOOST_TEST(w.ev.updatesFor(id) <= 5u);
}

BOOST_AUTO_TEST_CASE(stats_lists_inputs)
{
    Config cfg = baseConfig();
    std::map<std::string, FakeInput*> in;
    EngineFx fx(cfg, fakeInputs(twoInputs(), in));
    fx.t.call([&] {
        in["a"]->connection(true);
        in["a"]->emit(from("a", 0x111111, 0));
        in["a"]->emit(from("a", 0x111111, 0.001));
        in["b"]->emit(from("b", 0x222222, 0));  // running, not connected
    });
    const auto s = fx.stats();
    BOOST_REQUIRE(s.inputs.size() == 2u);
    BOOST_TEST(s.inputs[0].id == "a");
    BOOST_TEST(s.inputs[0].connected);
    BOOST_TEST(s.inputs[0].observations == 2u);
    BOOST_TEST(s.inputs[1].id == "b");
    BOOST_TEST(!s.inputs[1].connected);
    BOOST_TEST(s.inputs[1].observations == 1u);
    const std::string line = fx.t.call([&] { return fx.engine->statsLine(); });
    BOOST_TEST(line.find(" inputs=1/2") != std::string::npos, line);
    BOOST_TEST(line.find(" dump978=off") != std::string::npos, line);
}

BOOST_AUTO_TEST_CASE(inputs_changed_event_on_connection_change)
{
    Config cfg = baseConfig();
    std::map<std::string, FakeInput*> in;
    EngineFx fx(cfg, fakeInputs(twoInputs(), in));
    int changes = 0;
    fx.t.call([&] {
        fx.engine->onEvent([&](const skycued::EngineEvent& e) {
            if (e.kind == skycued::EventKind::inputs_changed)
                ++changes;
        });
        in["a"]->connection(true);
    });
    BOOST_TEST(fx.t.call([&] { return changes; }) == 1);
    fx.t.call([&] { in["a"]->connection(true); });  // no change: no event
    BOOST_TEST(fx.t.call([&] { return changes; }) == 1);
    fx.t.call([&] { in["a"]->connection(false); });
    BOOST_TEST(fx.t.call([&] { return changes; }) == 2);
    fx.t.call([&] { fx.engine->inputs().setEnabled("b", false); });
    BOOST_TEST(fx.t.call([&] { return changes; }) == 3);
    fx.t.call([&] { fx.engine->onEvent({}); });
}

BOOST_AUTO_TEST_CASE(engine_requires_inputs)
{
    IoThread t;
    t.call([&] {
        ForwarderConfig fc;
        fc.tcp_listen_ephemeral = true;
        Forwarder fwd(t.io, fc);
        BOOST_CHECK_THROW(Engine(t.io, baseConfig(), nullptr, fwd, nullptr, nullptr), std::invalid_argument);
    });
}

BOOST_AUTO_TEST_CASE(builtin_enabled_again_restarts)
{
    Config cfg = baseConfig();
    std::map<std::string, FakeInput*> in;
    EngineFx fx(cfg, fakeInputs(twoInputs(), in));
    fx.t.call([&] {
        fx.engine->inputs().setEnabled("a", false);
        fx.engine->inputs().setEnabled("a", true);
        in["a"]->connection(true);
    });
    BOOST_TEST(fx.t.call([&] { return in["a"]->starts; }) == 2);
    BOOST_TEST(fx.t.call([&] { return fx.engine->inputs().anyTrackInputConnected(); }));
}

// ---- drop counters per filter decision, enriched drop events ----

BOOST_AUTO_TEST_CASE(filtered_drops_counted_by_decision)
{
    Config cfg = baseConfig();  // max range 150 nm, min elevation 0
    std::map<std::string, FakeInput*> in;
    EngineFx fx(cfg, fakeInputs({fakeDef("a")}, in));
    fx.t.call([&] {
        in["a"]->connection(true);
        in["a"]->emit(from("a", 0xa00001, 0));    // about 3 nm
        in["a"]->emit(from("a", 0xa00002, 0.1));  // about 9 nm
    });
    BOOST_TEST(fx.stats().tracks_forwarded == 2u);

    fx.t.call([&] { fx.engine->addListEntry(skycued::ListKind::block, "a00001", "test"); });
    BOOST_TEST(fx.stats().drops_blocked == 1u);

    fx.t.call([&] { fx.engine->setFilterConfig(skycued::FilterConfig{0.5, 0}, "test"); });
    BOOST_TEST(fx.stats().drops_out_of_range == 1u);

    fx.t.call([&] { fx.engine->setFilterConfig(skycued::FilterConfig{150, 0}, "test"); });
    BOOST_TEST(fx.stats().tracks_forwarded == 1u);  // 0xa00002 again; 0xa00001 is still blocked
    fx.t.call([&] { fx.engine->setFilterConfig(skycued::FilterConfig{150, 89}, "test"); });
    BOOST_TEST(fx.stats().drops_below_elevation == 1u);

    const auto s = fx.stats();
    BOOST_TEST(s.drops_filtered == 3u);
    BOOST_TEST(s.drops_filtered == s.drops_blocked + s.drops_out_of_range + s.drops_below_elevation + s.drops_no_position);
    BOOST_TEST(s.drops_no_position == 0u);
    BOOST_TEST(s.sent_drops == 3u);
}

BOOST_AUTO_TEST_CASE(drop_event_carries_decision_callsign_time)
{
    Config cfg = baseConfig();
    cfg.track_timeout_s = 1;
    std::mutex m;  // declared before fx: they outlive the engine's sink
    std::condition_variable cv;
    std::vector<skycued::EngineEvent> drops;
    std::map<std::string, FakeInput*> in;
    EngineFx fx(cfg, fakeInputs({fakeDef("a")}, in));
    const TrackId one{0xa00001, AddressType::icao}, two{0xa00002, AddressType::icao};
    std::chrono::system_clock::time_point observed_at{};
    fx.t.call([&] {
        fx.engine->onEvent([&](const skycued::EngineEvent& e) {
            if (e.kind != skycued::EventKind::drop)
                return;
            std::lock_guard<std::mutex> lk(m);
            drops.push_back(e);
            cv.notify_all();
        });
        in["a"]->connection(true);
        auto o1 = from("a", one.address, 0);
        o1.callsign = std::string("TEST1");
        observed_at = o1.received_time;
        in["a"]->emit(o1);
        auto o2 = from("a", two.address, 0.1);
        o2.callsign = std::string("TEST2");
        in["a"]->emit(o2);
    });
    BOOST_TEST(fx.stats().tracks_forwarded == 2u);

    fx.t.call([&] { fx.engine->addListEntry(skycued::ListKind::block, "a00001", "test"); });
    {
        std::lock_guard<std::mutex> lk(m);
        BOOST_REQUIRE(drops.size() == 1u);
        const auto& d = drops[0];
        BOOST_REQUIRE(d.id.has_value());
        BOOST_TEST((*d.id == one));
        BOOST_TEST((d.reason == DropReason::filtered));
        BOOST_TEST((d.decision == skycued::Decision::blocked));
        BOOST_TEST((d.callsign == std::string("TEST1")));
        BOOST_REQUIRE(d.time.has_value());
        BOOST_TEST((*d.time >= observed_at));
    }

    // No more observations for 0xa00002: tick()'s expiry path drops it.
    std::unique_lock<std::mutex> lk(m);
    BOOST_REQUIRE(cv.wait_for(lk, 3s, [&] { return drops.size() >= 2; }));
    const auto& d = drops[1];
    BOOST_REQUIRE(d.id.has_value());
    BOOST_TEST((*d.id == two));
    BOOST_TEST((d.reason == DropReason::timeout));
    BOOST_TEST(!d.decision.has_value());
    BOOST_TEST((d.callsign == std::string("TEST2")));
    BOOST_TEST(d.time.has_value());
    lk.unlock();
    fx.t.call([&] { fx.engine->onEvent({}); });
}

BOOST_AUTO_TEST_CASE(filter_history_ring)
{
    FakeSource* src = nullptr;
    EngineFx fx(baseConfig(), fakeSource(src));  // no record dir: no sent recorder
    fx.t.call([&] {
        auto h = fx.engine->filterHistory();
        BOOST_REQUIRE(h.size() == 1u);
        BOOST_TEST(h[0].origin == "startup");
        BOOST_TEST(h[0].filters.max_range_nm == 150.0);
        BOOST_TEST(h[0].filters.min_elevation_deg == 0.0);

        for (int i = 0; i < 120; ++i)
            fx.engine->setFilterConfig({10.0 + i, 0}, "ui");
        h = fx.engine->filterHistory();
        BOOST_REQUIRE(h.size() == skycued::kFilterHistoryMax);
        BOOST_TEST(h.front().filters.max_range_nm == 30.0);  // i = 20
        BOOST_TEST(h.back().filters.max_range_nm == 129.0);
        for (size_t k = 0; k < h.size(); ++k) {
            BOOST_TEST(h[k].origin == "ui");
            if (k > 0)
                BOOST_TEST((h[k - 1].time <= h[k].time));
        }
    });
}

BOOST_AUTO_TEST_CASE(filter_history_load_before_start)
{
    IoThread t;
    std::unique_ptr<Forwarder> fwd;
    std::unique_ptr<Engine> engine;
    FakeSource* src = nullptr;
    const auto make = fakeSource(src);
    t.call([&] {
        ForwarderConfig fc;
        fc.tcp_listen_ephemeral = true;
        fwd = std::make_unique<Forwarder>(t.io, fc);
        engine = std::make_unique<Engine>(t.io, baseConfig(), sourceInputs(t.io, make(t.io)), *fwd, nullptr, nullptr);
        engine->setFilterConfig({40, 1}, "startup");  // as RuntimeConfig::load() with an overlay
        engine->start();
        const auto h = engine->filterHistory();
        BOOST_REQUIRE(h.size() == 1u);
        BOOST_TEST(h[0].origin == "startup");
        BOOST_TEST(h[0].filters.max_range_nm == 40.0);
        BOOST_TEST(h[0].filters.min_elevation_deg == 1.0);
        engine.reset();
        fwd.reset();
    });
    t.join();
}
