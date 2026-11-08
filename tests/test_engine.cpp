#define BOOST_TEST_MODULE skycued_engine
#include <boost/test/unit_test.hpp>

#include "config.hpp"
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
#include <cmath>
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
    cfg.allow_list_path = "";  // allow list empty
    cfg.block_list_path = tmp.path + "/block.txt";
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
    cfg.block_list_path = tmp.path + "/block.txt";
    cfg.allow_list_path = tmp.path + "/allow.txt";
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
    // must not make those fields flap and so bypass cues.min_interval_s.
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
    // cues.max_interval_s resends must still happen. Timing-tolerant (no clock
    // can be injected into the engine cheaply): it waits up to 8 s for two
    // resends, which the stalled clock never sends at all, and checks they
    // are max-interval resends, not at the 100 ms observation rate.
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
    const auto end = steady::now() + 8s;
    auto resends = [&] {
        std::lock_guard<std::mutex> lk(w.ev.m);
        return w.ev.updatesFor(id);
    };
    while (steady::now() < end && resends() < 3) {
        TrackObservation o = nearbyObservation(id.address, 0);  // same state: only max_interval sends
        o.received_time -= 500ms;
        o.position_time = o.received_time;
        fx.t.call([&] { fake->emit(o); });
        std::this_thread::sleep_for(100ms);
    }
    std::lock_guard<std::mutex> lk(w.ev.m);
    BOOST_REQUIRE(w.ev.updatesFor(id) >= 3u);  // the first plus two resends
    for (size_t k = 1; k < w.ev.update_times.size(); ++k)  // only this track is sent
        BOOST_TEST(std::chrono::duration<double>(w.ev.update_times[k] - w.ev.update_times[k - 1]).count() > 0.5);
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

BOOST_AUTO_TEST_CASE(filter_history_uses_the_wall_clock_during_a_replay)
{
    FakeSource* src = nullptr;
    EngineFx fx(baseConfig(), fakeSource(src));
    fx.t.call([&] {
        TrackObservation o = nearbyObservation(0x777777, 0);
        o.received_time = std::chrono::system_clock::from_time_t(1420070400);  // 2015: a replay's data clock
        o.position_time = o.received_time;
        src->connection(true);
        src->emit(o);
        BOOST_TEST((fx.engine->snapshot().now < std::chrono::system_clock::from_time_t(1500000000)));  // the data clock is in 2015
        fx.engine->setFilterConfig({20, 0}, "ui");
        const auto h = fx.engine->filterHistory();
        BOOST_REQUIRE(h.size() == 2u);
        const auto wall = std::chrono::system_clock::now();
        for (const auto& c : h)
            BOOST_TEST(std::chrono::duration<double>(wall - c.time).count() < 60.0);
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

BOOST_AUTO_TEST_CASE(engine_counts_rebroadcast_suppressed)
{
    FakeSource* src = nullptr;
    EngineFx fx(baseConfig(), fakeSource(src));
    int observed = 0;
    fx.t.call([&] {
        fx.engine->onEvent([&](const skycued::EngineEvent& e) {
            if (e.kind == skycued::EventKind::observed)
                ++observed;
        });
        src->connection(true);
        TrackObservation d = nearbyObservation(0x777777, 0);
        src->emit(d);
        TrackObservation r = nearbyObservation(0x777777, 0.01);
        r.rebroadcast = true;
        r.source = skycue::Source::tisb;
        r.received_time = d.received_time + 100ms;
        r.position_time = r.received_time;
        src->emit(r);
    });
    BOOST_TEST(fx.stats().rebroadcast_suppressed == 1u);
    // The suppressed position is not a fresh one: only the direct one is observed.
    BOOST_TEST(fx.t.call([&] { return observed; }) == 1);
    fx.t.call([&] { fx.engine->onEvent({}); });
}

// ---- geometric altitude: reported or estimated, applied when a track is built ----

namespace {

using Tp = std::chrono::system_clock::time_point;

// The truth the synthetic population follows: GNSS minus baro = kA + kB * baro.
constexpr double kTruthA = 150, kTruthB = 0.02;
int32_t truthDiff(int32_t baro) { return static_cast<int32_t>(std::lround(kTruthA + kTruthB * baro)); }

TrackObservation geomObs(uint32_t address, Tp t, int32_t baro, bool with_geom, double lat_offset)
{
    TrackObservation o = nearbyObservation(address, lat_offset);
    o.received_time = t;
    o.position_time = t;
    o.alt_baro_ft = baro;
    if (with_geom)
        o.alt_geom_ft = baro + truthDiff(baro);
    return o;
}

// Six aircraft 6000 ft apart, each reporting four paired baro+geom altitudes 6 s
// apart (24 samples, 6 aircraft, 30000 ft of span). Returns the time of the last.
Tp seedFit(FakeSource& src, Tp t0)
{
    Tp last = t0;
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 6; ++i) {
            last = t0 + std::chrono::seconds(6 * k);
            src.emit(geomObs(0x300000 + static_cast<uint32_t>(i), last, 12000 + 6000 * i, true, 0.01 * i));
        }
    return last;
}

Config geomConfig()
{
    Config cfg = baseConfig();
    cfg.min_interval_s = 0.05;
    cfg.track_timeout_s = 3600;
    return cfg;
}

// Every update the engine sent, in order (io thread only).
struct UpdateLog {
    std::vector<skycued::TrackSnapshot> updates;
    void attach(Engine& e)
    {
        e.onEvent([this](const skycued::EngineEvent& ev) {
            if (ev.kind == skycued::EventKind::update && ev.track)
                updates.push_back(*ev.track);
        });
    }
    const skycued::TrackSnapshot* last(TrackId id) const
    {
        for (auto it = updates.rbegin(); it != updates.rend(); ++it)
            if (it->track.id == id)
                return &*it;
        return nullptr;
    }
};

const skycued::TrackSnapshot* findTrack(const skycued::Snapshot& s, TrackId id)
{
    for (const auto& t : s.tracks)
        if (t.track.id == id)
            return &t;
    return nullptr;
}

Tp hoursAgo() { return std::chrono::system_clock::now() - std::chrono::hours(2); }

}  // namespace

BOOST_AUTO_TEST_CASE(reported_geom_track_unchanged)
{
    FakeSource* src = nullptr;
    EngineFx fx(geomConfig(), fakeSource(src));
    UpdateLog log;
    const TrackId id{0x401001, AddressType::icao};
    const Tp t0 = hoursAgo();
    TrackObservation o = geomObs(id.address, t0, 30000, true, 0);
    fx.t.call([&] {
        log.attach(*fx.engine);
        src->connection(true);
        src->emit(o);
    });
    const auto got = fx.t.call([&] { return log.last(id) ? std::optional<Track>(log.last(id)->track) : std::nullopt; });
    BOOST_REQUIRE(got.has_value());
    // What the encoder produced before estimates existed: the reported fields, no bit 10.
    Track want;
    want.id = id;
    want.source = o.source;
    want.position_time = *o.position_time;
    want.lat_deg = *o.lat;
    want.lon_deg = *o.lon;
    want.alt_is_geom = true;
    want.alt_baro_ft = o.alt_baro_ft;
    want.alt_geom_ft = o.alt_geom_ft;
    want.ground_speed_kt = static_cast<float>(*o.gs_kt);
    want.track_deg = static_cast<float>(*o.track_deg);
    BOOST_TEST(skycue::encodeTrackUpdate(*got) == skycue::encodeTrackUpdate(want), boost::test_tools::per_element());
    BOOST_TEST(!got->alt_geom_uncertainty_ft.has_value());
    const auto snap = fx.t.call([&] { return fx.engine->snapshot(); });
    const auto* t = findTrack(snap, id);
    BOOST_REQUIRE(t != nullptr);
    BOOST_TEST((t->geom.source == skycued::GeomSource::reported));
    BOOST_TEST(!t->geom.uncertainty_ft.has_value());
}

BOOST_AUTO_TEST_CASE(estimate_after_reported_expires)
{
    FakeSource* src = nullptr;
    EngineFx fx(geomConfig(), fakeSource(src));
    UpdateLog log;
    const TrackId id{0x401002, AddressType::icao};
    const Tp t0 = hoursAgo();
    fx.t.call([&] {
        log.attach(*fx.engine);
        src->connection(true);
        src->emit(geomObs(id.address, t0, 30000, true, 0));  // reported, diff truthDiff(30000)
    });
    std::this_thread::sleep_for(100ms);  // past cues.min_interval_s
    fx.t.call([&] {
        // 61 s later, baro only, moved: the reported value is too old to stand
        src->emit(geomObs(id.address, t0 + 61s, 30000, false, 0.01));
    });
    fx.t.call([&] {
        const auto* last = log.last(id);
        BOOST_REQUIRE(last != nullptr);
        BOOST_REQUIRE(log.updates.size() >= 2u);
        BOOST_TEST((last->geom.source == skycued::GeomSource::estimated));
        BOOST_TEST((last->track.alt_geom_ft == std::optional<int32_t>(30000 + truthDiff(30000))));
        BOOST_REQUIRE(last->track.alt_geom_uncertainty_ft.has_value());
        BOOST_TEST(*last->track.alt_geom_uncertainty_ft >= 75);
        BOOST_TEST(*last->track.alt_geom_uncertainty_ft <= 325);
        // The first update carried the reported value and no uncertainty.
        BOOST_TEST(!log.updates.front().track.alt_geom_uncertainty_ft.has_value());
    });
}

BOOST_AUTO_TEST_CASE(fit_learns_from_other_aircraft)
{
    FakeSource* src = nullptr;
    EngineFx fx(geomConfig(), fakeSource(src));
    UpdateLog log;
    const TrackId id{0x401003, AddressType::icao};
    const Tp t0 = hoursAgo();
    fx.t.call([&] {
        log.attach(*fx.engine);
        src->connection(true);
        const Tp last = seedFit(*src, t0);
        src->emit(geomObs(id.address, last + 10s, 33000, false, 0.02));  // baro only, past the 5 s fit cache of the seed
    });
    const auto snap = fx.t.call([&] { return fx.engine->snapshot(); });
    const auto* t = findTrack(snap, id);
    BOOST_REQUIRE(t != nullptr);
    BOOST_TEST((t->geom.source == skycued::GeomSource::estimated));
    BOOST_REQUIRE(t->track.alt_geom_ft.has_value());
    BOOST_TEST(std::abs(*t->track.alt_geom_ft - (33000 + truthDiff(33000))) <= 100);
    BOOST_REQUIRE(t->track.alt_geom_uncertainty_ft.has_value());
    BOOST_TEST(*t->track.alt_geom_uncertainty_ft >= 100);
    const auto st = fx.stats();
    BOOST_TEST(st.altitude_fit.valid);
    BOOST_TEST(st.altitude_fit.aircraft == 6u);
    BOOST_TEST(st.altitude_fit.samples >= 20u);
}

BOOST_AUTO_TEST_CASE(carried_diff_is_one_fit_sample)
{
    // Three aircraft, each sending 12 observations that carry one difference with its own
    // time: one fit sample per aircraft, however often the difference returns.
    FakeSource* src = nullptr;
    EngineFx fx(geomConfig(), fakeSource(src));
    const Tp t0 = hoursAgo();
    fx.t.call([&] {
        src->connection(true);
        for (int i = 0; i < 12; ++i)
            for (uint32_t a = 0; a < 3; ++a) {
                TrackObservation o = geomObs(0x310000 + a, t0 + std::chrono::milliseconds(100 * i), 30000 + 10 * i, false,
                                             0.01 * a);
                o.geom_minus_baro_ft = 400 + 50 * static_cast<int32_t>(a);
                o.geom_minus_baro_time = t0;
                src->emit(o);
            }
        // The fit is cached for 5 s of the data clock: a later observation refreshes it.
        src->emit(geomObs(0x310009, t0 + 10s, 30000, false, 0.09));
    });
    const auto st = fx.stats();
    BOOST_TEST(st.altitude_fit.valid);
    BOOST_TEST(st.altitude_fit.aircraft == 3u);
    BOOST_TEST(st.altitude_fit.samples == 3u);
}

BOOST_AUTO_TEST_CASE(uncertainty_clamped_at_65535)
{
    // Three aircraft whose differences are 100000 ft apart give a median fit with a p90 far
    // above the u16 of the wire; makeTrack clamps it.
    FakeSource* src = nullptr;
    EngineFx fx(geomConfig(), fakeSource(src));
    const TrackId id{0x401099, AddressType::icao};
    const Tp t0 = hoursAgo();
    fx.t.call([&] {
        src->connection(true);
        for (uint32_t a = 0; a < 3; ++a) {
            TrackObservation o = geomObs(0x320000 + a, t0, 30000, false, 0.01 * a);
            o.geom_minus_baro_ft = static_cast<int32_t>(100000 * a);
            o.geom_minus_baro_time = t0;
            src->emit(o);
        }
        src->emit(geomObs(id.address, t0 + 10s, 30000, false, 0.05));  // baro only, after the 5 s fit cache
    });
    const auto snap = fx.t.call([&] { return fx.engine->snapshot(); });
    const auto* t = findTrack(snap, id);
    BOOST_REQUIRE(t != nullptr);
    BOOST_TEST((t->geom.source == skycued::GeomSource::estimated));
    BOOST_TEST(*t->geom.uncertainty_ft > 65535);
    BOOST_REQUIRE(t->track.alt_geom_uncertainty_ft.has_value());
    BOOST_TEST(*t->track.alt_geom_uncertainty_ft == 65535);
}

BOOST_AUTO_TEST_CASE(tisb_target_gets_estimate)
{
    FakeSource* src = nullptr;
    EngineFx fx(geomConfig(), fakeSource(src));
    UpdateLog log;
    const TrackId id{0x2c4a20, AddressType::non_icao};
    const Tp t0 = hoursAgo();
    fx.t.call([&] {
        log.attach(*fx.engine);
        src->connection(true);
        const Tp last = seedFit(*src, t0);
        TrackObservation o = geomObs(id.address, last + 10s, 25000, false, 0.03);
        o.id = id;
        o.source = skycue::Source::tisb;
        o.rebroadcast = true;
        src->emit(o);
    });
    fx.t.call([&] {
        const auto* last = log.last(id);
        BOOST_REQUIRE(last != nullptr);  // sent, with an estimate
        BOOST_TEST((last->geom.source == skycued::GeomSource::estimated));
        BOOST_REQUIRE(last->track.alt_geom_ft.has_value());
        BOOST_TEST(std::abs(*last->track.alt_geom_ft - (25000 + truthDiff(25000))) <= 100);
        BOOST_TEST(last->track.alt_geom_uncertainty_ft.has_value());
    });
}

BOOST_AUTO_TEST_CASE(track_without_baro_has_no_geom)
{
    FakeSource* src = nullptr;
    EngineFx fx(geomConfig(), fakeSource(src));
    const TrackId none{0x401005, AddressType::icao}, stale{0x401006, AddressType::icao};
    const Tp t0 = hoursAgo();
    fx.t.call([&] {
        src->connection(true);
        const Tp last = seedFit(*src, t0);
        TrackObservation o = geomObs(none.address, last + 1s, 0, false, 0.02);
        o.alt_baro_ft.reset();  // no altitude at all
        src->emit(o);
        src->emit(geomObs(stale.address, last + 1s, 30000, false, 0.03));
        // 30 s on, another aircraft moves the data clock: stale's baro is 30 s old
        src->emit(geomObs(0x300000, last + 31s, 12000, true, 0));
    });
    const auto snap = fx.t.call([&] { return fx.engine->snapshot(); });
    for (const auto& id : {none, stale}) {
        const auto* t = findTrack(snap, id);
        BOOST_REQUIRE(t != nullptr);
        BOOST_TEST((t->geom.source == skycued::GeomSource::none));
        BOOST_TEST(!t->track.alt_geom_ft.has_value());
        BOOST_TEST(!t->track.alt_geom_uncertainty_ft.has_value());
    }
}

BOOST_AUTO_TEST_CASE(replay_uses_data_clock_for_window)
{
    // The recorded times are two hours in the past; the 120 s window is on them.
    FakeSource* src = nullptr;
    EngineFx fx(geomConfig(), fakeSource(src));
    const Tp t0 = hoursAgo();
    Tp last;
    fx.t.call([&] {
        src->connection(true);
        last = seedFit(*src, t0);
    });
    BOOST_TEST(fx.stats().altitude_fit.valid);  // a wall-clock window would have none of them
    BOOST_TEST(fx.stats().altitude_fit.age_s < 60.0);
    fx.t.call([&] { src->emit(geomObs(0x401007, last + 130s, 20000, false, 0.05)); });
    BOOST_TEST(!fx.stats().altitude_fit.valid);  // recorded time moved past the window
}

BOOST_AUTO_TEST_CASE(estimate_does_not_touch_received_recording)
{
    TempDir tmp;
    const TrackId id{0x401008, AddressType::icao};
    {
        FakeSource* src = nullptr;
        EngineFx fx(geomConfig(), fakeSource(src), 10s, tmp.path);
        UpdateLog log;
        const Tp t0 = hoursAgo();
        fx.t.call([&] {
            log.attach(*fx.engine);
            src->connection(true);
            const Tp last = seedFit(*src, t0);
            src->emit(geomObs(id.address, last + 10s, 33000, false, 0.02));
        });
        fx.t.call([&] {
            const auto* last = log.last(id);
            BOOST_REQUIRE(last != nullptr);
            BOOST_TEST(last->track.alt_geom_uncertainty_ft.has_value());  // estimated on the wire
        });
        fx.stop();
    }
    const std::string received = streamText(tmp.path, "received");
    BOOST_TEST(countOf(received, "\"type\":\"obs\"") == 25u);
    BOOST_TEST(countOf(received, "\"alt_geom_ft\"") == 24u);  // the seeds' reported values only
    const std::string sent = streamText(tmp.path, "sent");
    BOOST_TEST(countOf(sent, "\"event\":\"update\"") >= 7u);
}
