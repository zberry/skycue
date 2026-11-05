#define BOOST_TEST_MODULE skycued_stats_engine
#include <boost/test/unit_test.hpp>

// The Engine's wiring of the StatsStore (spec §4): what feeds it, when it
// is saved, loaded and reset.

#include "engine_events.hpp"
#include "engine_test_support.hpp"
#include "filters.hpp"
#include "inputs/dump978_input.hpp"
#include "restricted_fixtures.hpp"
#include "stats_store.hpp"
#include "time_util.hpp"

#include <sys/stat.h>

#include <numeric>

using namespace enginetest;
using skycued::StatsLoad;
using skycued::StatsRes;
using skycued::StatsStore;

namespace {

bool exists(const std::string& path)
{
    struct ::stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

// The file's mtime in nanoseconds; -1 when it does not exist.
long long mtimeNs(const std::string& path)
{
    struct ::stat st{};
    if (::stat(path.c_str(), &st) != 0)
        return -1;
    return static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
}

template <class Pred> bool waitUntil(Pred p, std::chrono::milliseconds timeout = kWait)
{
    const auto end = steady::now() + timeout;
    while (steady::now() < end) {
        if (p())
            return true;
        std::this_thread::sleep_for(20ms);
    }
    return p();
}

std::size_t ledgerSize(EngineFx& fx)
{
    return fx.t.call([&] { return fx.engine->statsStore().aircraftCount(); });
}

// A fresh store with `path` loaded into it.
std::size_t aircraftInFile(const std::string& path)
{
    StatsStore s(skycued::ReceiverPosition{38.9, -77.0, 0});
    BOOST_REQUIRE(skycued::loadStatsFile(path, s) == StatsLoad::loaded);
    return s.aircraftCount();
}

const TrackId kId{0xA00001, AddressType::icao};

}  // namespace

BOOST_AUTO_TEST_CASE(observation_and_send_counted)
{
    FakeSource* src = nullptr;
    EngineFx fx(baseConfig(), fakeSource(src));
    fx.t.call([&] { src->emit(nearbyObservation(0xA00001, 0)); });
    fx.t.call([&] {
        const StatsStore& s = fx.engine->statsStore();
        BOOST_REQUIRE(!s.series(StatsRes::minute).empty());
        const auto& b = s.series(StatsRes::minute).back();
        BOOST_TEST(b.observations == 1u);
        BOOST_TEST(b.positions == 1u);
        BOOST_TEST(b.sent_updates == 1u);
        const auto* a = s.aircraft(kId);
        BOOST_REQUIRE(a);
        BOOST_TEST(a->sent_updates == 1u);
        BOOST_TEST(a->observations == 1u);
        BOOST_TEST(s.dirty());
    });
    BOOST_TEST(fx.t.call([&] { return fx.engine->statsPath(); }).empty());  // no loadStats: not saved
}

BOOST_AUTO_TEST_CASE(filtered_drop_counted)
{
    FakeSource* src = nullptr;
    EngineFx fx(baseConfig(), fakeSource(src));
    fx.t.call([&] { src->emit(nearbyObservation(0xA00001, 0)); });  // ~3 nm out: forwarded
    fx.t.call([&] { fx.engine->setFilterConfig(skycued::FilterConfig{1, 0}, "test"); });
    fx.t.call([&] {
        const StatsStore& s = fx.engine->statsStore();
        const auto& b = s.series(StatsRes::minute).back();
        BOOST_TEST(b.drops_filtered == 1u);
        BOOST_TEST(b.drops_out_of_range == 1u);
        BOOST_TEST(b.drops_timeout == 0u);
        const auto* a = s.aircraft(kId);
        BOOST_REQUIRE(a);
        BOOST_TEST(a->drops == 1u);
    });
}

BOOST_AUTO_TEST_CASE(timeout_drop_counted_shutdown_not)
{
    TempDir dir;
    const std::string path = dir.path + "/s.json";
    FakeSource* src = nullptr;
    Config cfg = baseConfig();
    cfg.track_timeout_s = 1;
    {
        EngineFx fx(cfg, fakeSource(src));
        fx.t.call([&] { fx.engine->loadStats(path, std::chrono::seconds(300)); });
        fx.t.call([&] { src->connection(true); });  // connected: an expiry is a timeout, not source_lost
        fx.t.call([&] { src->emit(nearbyObservation(0xA00001, 0)); });
        BOOST_REQUIRE(waitUntil([&] { return fx.stats().drops_timeout == 1u; }));
        fx.t.call([&] {
            BOOST_TEST(fx.engine->statsStore().series(StatsRes::minute).back().drops_timeout == 1u);
            BOOST_TEST(fx.engine->statsStore().aircraft(kId)->drops == 1u);
        });
        fx.t.call([&] { src->emit(nearbyObservation(0xA00002, 0)); });  // forwarded at stop()
        fx.stop();
    }
    StatsStore s(skycued::ReceiverPosition{38.9, -77.0, 0});
    BOOST_REQUIRE(skycued::loadStatsFile(path, s) == StatsLoad::loaded);
    const auto& m = s.series(StatsRes::minute);
    const uint64_t drops = std::accumulate(m.begin(), m.end(), uint64_t{0}, [](uint64_t n, const auto& b) {
        return n + b.drops_timeout + b.drops_filtered + b.drops_source_lost;
    });
    BOOST_TEST(drops == 1u);  // the shutdown drop is not counted
    BOOST_TEST(s.aircraft(TrackId{0xA00002, AddressType::icao})->drops == 0u);
}

BOOST_AUTO_TEST_CASE(stop_saves_and_reload_restores)
{
    TempDir dir;
    const std::string path = dir.path + "/s.json";
    {
        FakeSource* src = nullptr;
        EngineFx fx(baseConfig(), fakeSource(src));
        fx.t.call([&] { fx.engine->loadStats(path, std::chrono::seconds(300)); });
        BOOST_TEST(fx.t.call([&] { return fx.engine->statsPath(); }) == path);
        fx.t.call([&] { src->emit(nearbyObservation(0xA00001, 0)); });
        BOOST_TEST(!exists(path));  // not yet: 300 s
        fx.stop();
        BOOST_TEST(exists(path));
        BOOST_TEST(!fx.t.call([&] { return fx.engine->statsStore().dirty(); }));
    }
    FakeSource* src = nullptr;
    EngineFx fx(baseConfig(), fakeSource(src));
    fx.t.call([&] { fx.engine->loadStats(path, std::chrono::seconds(300)); });
    BOOST_TEST(ledgerSize(fx) == 1u);
    fx.t.call([&] {
        const auto* a = fx.engine->statsStore().aircraft(kId);
        BOOST_REQUIRE(a);
        BOOST_TEST(a->sent_updates == 1u);
        BOOST_TEST(fx.engine->statsStore().receiver().has_value());
        BOOST_TEST(fx.engine->statsStore().limits().aircraft_max == 20000u);
    });
}

BOOST_AUTO_TEST_CASE(aircraft_max_from_config)
{
    FakeSource* src = nullptr;
    Config cfg = baseConfig();
    cfg.stats_aircraft_max = 150;
    EngineFx fx(cfg, fakeSource(src));
    BOOST_TEST(fx.t.call([&] { return fx.engine->statsStore().limits().aircraft_max; }) == 150u);
    TempDir dir;
    fx.t.call([&] { fx.engine->loadStats(dir.path + "/none.json", std::chrono::seconds(300)); });  // missing
    BOOST_TEST(fx.t.call([&] { return fx.engine->statsStore().limits().aircraft_max; }) == 150u);
}

BOOST_AUTO_TEST_CASE(periodic_save_when_dirty)
{
    TempDir dir;
    const std::string path = dir.path + "/s.json";
    FakeSource* src = nullptr;
    Config cfg = baseConfig();
    cfg.track_timeout_s = 60;  // no timeout drop (a change) inside the check below
    EngineFx fx(cfg, fakeSource(src));
    fx.t.call([&] { fx.engine->loadStats(path, std::chrono::seconds(0)); });
    // The data clock starts 5 s into a minute, so the check stays inside one
    // minute bucket (and one hour bucket) of the data clock.
    TrackObservation o = nearbyObservation(0xA00001, 0);
    const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(o.received_time.time_since_epoch()).count();
    o.received_time = std::chrono::system_clock::time_point(std::chrono::seconds(now_s - now_s % 60 + 5));
    o.position_time = o.received_time;
    fx.t.call([&] { src->emit(o); });
    BOOST_REQUIRE(waitUntil([&] { return exists(path); }, slow(3s)));
    BOOST_REQUIRE(waitUntil([&] { return !fx.t.call([&] { return fx.engine->statsStore().dirty(); }); }, slow(3s)));
    const long long before = mtimeNs(path);
    const auto bucket = fx.t.call([&] { return fx.engine->statsStore().series(StatsRes::minute).back().start_s; });
    std::this_thread::sleep_for(2500ms);  // two ticks or more, no change
    BOOST_TEST(fx.t.call([&] { return fx.engine->statsStore().series(StatsRes::minute).back().start_s; }) == bucket);
    BOOST_TEST(mtimeNs(path) == before);
    BOOST_TEST(aircraftInFile(path) == 1u);
    // A change is saved on the next tick.
    fx.t.call([&] { src->emit(nearbyObservation(0xA00002, 0)); });
    BOOST_TEST(waitUntil([&] { return mtimeNs(path) != before; }, slow(3s)));
}

BOOST_AUTO_TEST_CASE(failed_save_is_retried)
{
    TempDir dir;
    const std::string path = dir.path + "/no-such-dir/s.json";
    FakeSource* src = nullptr;
    EngineFx fx(baseConfig(), fakeSource(src));
    fx.t.call([&] { fx.engine->loadStats(path, std::chrono::seconds(0)); });
    fx.t.call([&] { src->emit(nearbyObservation(0xA00001, 0)); });
    std::this_thread::sleep_for(1500ms);
    BOOST_TEST(!exists(path));
    BOOST_TEST(fx.t.call([&] { return fx.engine->statsStore().dirty(); }));  // still to save
    BOOST_REQUIRE(::mkdir((dir.path + "/no-such-dir").c_str(), 0700) == 0);
    BOOST_TEST(waitUntil([&] { return exists(path); }, slow(3s)));
}

BOOST_AUTO_TEST_CASE(empty_path_disables_persistence)
{
    TempDir dir;
    const bool had_default = exists("skycue.stats.json");
    {
        FakeSource* src = nullptr;
        EngineFx fx(baseConfig(), fakeSource(src));
        fx.t.call([&] { fx.engine->loadStats("", std::chrono::seconds(0)); });
        fx.t.call([&] { src->emit(nearbyObservation(0xA00001, 0)); });
        std::this_thread::sleep_for(1500ms);
        fx.t.call([&] { fx.engine->resetStats("test"); });
        fx.t.call([&] { src->emit(nearbyObservation(0xA00001, 0)); });
        fx.stop();
        BOOST_TEST(fx.t.call([&] { return fx.engine->statsPath(); }).empty());
    }
    std::vector<std::string> entries = filesWithSuffix(dir.path, "");
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const std::string& e) { return e == dir.path + "/." || e == dir.path + "/.."; }),
                  entries.end());
    BOOST_TEST(entries.empty());
    BOOST_TEST(exists("skycue.stats.json") == had_default);  // the config default is not used on its own
}

BOOST_AUTO_TEST_CASE(reset_empties_and_saves)
{
    TempDir dir;
    const std::string path = dir.path + "/s.json";
    FakeSource* src = nullptr;
    EngineFx fx(baseConfig(), fakeSource(src));
    fx.t.call([&] { fx.engine->loadStats(path, std::chrono::seconds(300)); });
    fx.t.call([&] { src->emit(nearbyObservation(0xA00001, 0)); });
    BOOST_TEST(ledgerSize(fx) == 1u);
    fx.t.call([&] { fx.engine->resetStats("test"); });
    BOOST_TEST(ledgerSize(fx) == 0u);
    fx.t.call([&] {
        const StatsStore& s = fx.engine->statsStore();
        BOOST_REQUIRE(s.sinceS().has_value());
        const auto clock_s =
            std::chrono::duration_cast<std::chrono::seconds>(fx.engine->dataClock().time_since_epoch()).count();
        BOOST_TEST(*s.sinceS() <= clock_s);
        BOOST_TEST(*s.sinceS() >= clock_s - 5);
        BOOST_TEST(!s.dirty());  // saved at once
    });
    BOOST_REQUIRE(exists(path));
    BOOST_TEST(aircraftInFile(path) == 0u);
}

BOOST_AUTO_TEST_CASE(fisb_products_counted, *skycue_test::needs_fixture("fisb/received-with-uplinks.jsonl"))
{
    // The uplinks of the FIS-B fixture, fed through one input after an
    // observation started the data clock and a tick ran.
    std::vector<std::string> hexes;
    {
        std::ifstream in(std::string(SKYCUE_FIXTURE_DIR) + "/fisb/received-with-uplinks.jsonl");
        const std::string key = "\"uplink_hex\":\"";
        for (std::string line; std::getline(in, line) && hexes.size() < 200;) {
            const auto p = line.find(key);
            if (p == std::string::npos)
                continue;
            const auto b = p + key.size();
            hexes.push_back(line.substr(b, line.find('"', b) - b));
        }
    }
    BOOST_REQUIRE(!hexes.empty());
    std::map<std::string, FakeInput*> in;
    EngineFx fx(baseConfig(), fakeInputs({fakeDef("ping", InputType::pingstation_json)}, in));
    fx.t.call([&] { in["ping"]->emit(nearbyObservation(0xA00001, 0)); });
    std::this_thread::sleep_for(1500ms);
    fx.t.call([&] {
        for (const auto& h : hexes)
            in["ping"]->uplink(std::chrono::system_clock::now(), h);
    });
    const uint64_t products = fx.stats().fisb_products;
    BOOST_REQUIRE(products > 0u);
    auto stored = [&] {
        return fx.t.call([&] {
            uint64_t n = 0;
            for (const auto& b : fx.engine->statsStore().series(StatsRes::minute))
                n += b.fisb_products;
            return n;
        });
    };
    BOOST_TEST(waitUntil([&] { return stored() == products; }, slow(3s)));
    BOOST_TEST(fx.t.call([&] { return fx.engine->statsStore().uatSeen(); }));
}

BOOST_AUTO_TEST_CASE(fisb_products_of_unpaced_replay_counted,
                     *skycue_test::needs_fixture("fisb/received-with-uplinks.jsonl"))
{
    // A replay of uplinks alone at speed 0 (as the UI suite's FIS-B daemons
    // run): every uplink is decoded before the engine's first tick, which
    // must still put the products in the series (the counters start at 0
    // with the engine, so there is no earlier count to skip).
    TempDir dir;
    const std::string rec = dir.path + "/uplinks.jsonl";
    {
        std::ifstream in(std::string(SKYCUE_FIXTURE_DIR) + "/fisb/received-with-uplinks.jsonl");
        std::ofstream out(rec);
        int n = 0;
        for (std::string line; std::getline(in, line) && n < 200;)
            if (line.find("\"type\":\"uat_uplink\"") != std::string::npos) {
                out << line << '\n';
                ++n;
            }
        BOOST_REQUIRE(n == 200);
    }
    EngineFx fx(baseConfig(), [rec](asio::io_context& io) -> std::unique_ptr<ObservationSource> {
        return std::make_unique<skycued::ReplayObservationSource>(io, rec, 0, false);
    });
    BOOST_REQUIRE(waitUntil([&] { return fx.stats().fisb_uplinks == 200u; }));
    const uint64_t products = fx.stats().fisb_products;
    BOOST_REQUIRE(products > 0u);
    auto stored = [&] {
        return fx.t.call([&] {
            uint64_t n = 0;
            for (const auto& b : fx.engine->statsStore().series(StatsRes::minute))
                n += b.fisb_products;
            return n;
        });
    };
    BOOST_TEST(waitUntil([&] { return stored() == products; }, slow(3s)));
    BOOST_TEST(stored() > 0u);
    BOOST_TEST(fx.t.call([&] { return fx.engine->statsStore().uatSeen(); }));
}

BOOST_AUTO_TEST_CASE(uat_frames_reach_the_series)
{
    // A real Dump978Input (EngineStats takes uat_downlink/uat_uplink from
    // it) beside a track input that starts the data clock.
    TestServer server;
    FakeInput* readsb = nullptr;
    auto make = [&](asio::io_context& io) {
        auto inputs = std::make_unique<InputManager>(io, skycued::InputManagerConfig{});
        auto r = std::make_unique<FakeInput>(fakeDef("readsb"));
        readsb = r.get();
        inputs->addBuiltin(std::move(r));
        InputDef uat = fakeDef("dump978", InputType::dump978);
        uat.port = server.port();
        uat.builtin = true;
        inputs->addBuiltin(std::make_unique<skycued::Dump978Input>(io, uat));
        inputs->load();
        return inputs;
    };
    EngineFx fx(baseConfig(), std::function<std::unique_ptr<InputManager>(asio::io_context&)>(make));
    auto sock = server.accept(kWait);
    BOOST_REQUIRE(sock);
    sock->non_blocking(false);
    fx.t.call([&] { readsb->emit(nearbyObservation(0xA00001, 0)); });
    std::this_thread::sleep_for(1500ms);  // a tick runs first
    const skycued::EngineStats before = fx.stats();
    std::string lines;
    for (int i = 0; i < 5; ++i)
        lines += "-4567;\n-89ab;\n+0123;\n";
    asio::write(*sock, asio::buffer(lines));
    BOOST_REQUIRE(waitUntil([&] { return fx.stats().uat_downlink == before.uat_downlink + 10; }));
    const skycued::EngineStats after = fx.stats();
    BOOST_TEST(after.uat_uplink == before.uat_uplink + 5);
    using skycued::StatsBucket;
    auto stored = [&](uint64_t StatsBucket::*f) {
        return fx.t.call([&] {
            uint64_t n = 0;
            for (const auto& b : fx.engine->statsStore().series(StatsRes::minute))
                n += b.*f;
            return n;
        });
    };
    BOOST_TEST(waitUntil([&] { return stored(&StatsBucket::uat_downlink) == 10u; }, slow(3s)));
    BOOST_TEST(stored(&StatsBucket::uat_downlink) == after.uat_downlink - before.uat_downlink);
    BOOST_TEST(stored(&StatsBucket::uat_uplink) == after.uat_uplink - before.uat_uplink);
    BOOST_TEST(fx.t.call([&] { return fx.engine->statsStore().uatSeen(); }));
}

namespace {

uint64_t storeSourceLost(EngineFx& fx)
{
    return fx.t.call([&] {
        uint64_t n = 0;
        for (const auto& b : fx.engine->statsStore().series(StatsRes::minute))
            n += b.drops_source_lost;
        return n;
    });
}

}  // namespace

BOOST_AUTO_TEST_CASE(source_lost_expiry_counted)
{
    // The track input never connected: a forwarded track that ages out goes
    // as source_lost (tick expiry), and the store counts it so.
    FakeSource* src = nullptr;
    Config cfg = baseConfig();
    cfg.track_timeout_s = 1;
    EngineFx fx(cfg, fakeSource(src));
    fx.t.call([&] { src->emit(nearbyObservation(0xA00001, 0)); });
    BOOST_REQUIRE(waitUntil([&] { return fx.stats().drops_source_lost == 1u; }));
    BOOST_TEST(storeSourceLost(fx) == 1u);
    fx.t.call([&] {
        const auto& m = fx.engine->statsStore().series(StatsRes::minute);
        uint64_t timeouts = 0;
        for (const auto& b : m)
            timeouts += b.drops_timeout;
        BOOST_TEST(timeouts == 0u);
        BOOST_TEST(fx.engine->statsStore().aircraft(kId)->drops == 1u);
    });
}

BOOST_AUTO_TEST_CASE(source_lost_drop_all_counted)
{
    // Connected, then lost for TRACK_TIMEOUT_S while the track stays fresh:
    // dropAllForwarded(source_lost) drops it, and the store counts it so.
    FakeSource* src = nullptr;
    Config cfg = baseConfig();
    cfg.track_timeout_s = 2;
    EngineFx fx(cfg, fakeSource(src));
    fx.t.call([&] { src->connection(true); });
    fx.t.call([&] { src->emit(nearbyObservation(0xA00001, 0)); });
    fx.t.call([&] { src->connection(false); });
    // Keep the track fresh so tick expiry never takes it: only the lost timer can.
    const auto end = steady::now() + slow(6s);
    while (steady::now() < end && fx.stats().drops_source_lost == 0u) {
        fx.t.call([&] { src->emit(nearbyObservation(0xA00001, 0)); });
        std::this_thread::sleep_for(200ms);
    }
    const uint64_t engine_lost = fx.stats().drops_source_lost;
    BOOST_REQUIRE(engine_lost >= 1u);
    BOOST_TEST(fx.stats().drops_timeout == 0u);
    BOOST_TEST(storeSourceLost(fx) == fx.stats().drops_source_lost);
    BOOST_TEST(fx.t.call([&] { return fx.engine->statsStore().aircraft(kId)->drops; }) >= 1u);
}

BOOST_AUTO_TEST_CASE(not_regular_stats_file_disables_persistence)
{
    TempDir dir;
    for (const std::string& path : {dir.path, std::string("/dev/null")}) {
        FakeSource* src = nullptr;
        EngineFx fx(baseConfig(), fakeSource(src));
        fx.t.call([&] { fx.engine->loadStats(path, std::chrono::seconds(0)); });
        BOOST_TEST(fx.t.call([&] { return fx.engine->statsPath(); }).empty(), path);
        fx.t.call([&] { src->emit(nearbyObservation(0xA00001, 0)); });
        std::this_thread::sleep_for(1500ms);  // a tick or more: no save is attempted
        fx.stop();
        BOOST_TEST(!exists(path + ".bad"), path);
        BOOST_TEST(exists(path), path);
    }
}
