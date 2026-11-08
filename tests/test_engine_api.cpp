#define BOOST_TEST_MODULE skycued_engine_api
#include <boost/test/unit_test.hpp>

#include "conf_support.hpp"
#include "engine_events.hpp"
#include "engine_test_support.hpp"
#include "filters.hpp"
#include "json_lite.hpp"
#include "decode/uat_decoder.hpp"
#include "runtime_config.hpp"
#include "uat_test_support.hpp"
#include "web_api.hpp"

#include <cmath>
#include <map>
#include <tuple>

using namespace enginetest;
using skycued::Decision;
using skycued::EngineEvent;
using skycued::EventKind;
using skycued::FilterConfig;
using skycued::ListKind;
using skycued::Snapshot;
using skycued::TrackSnapshot;

namespace {

// EngineEvents captured by the sink; only touched on the io thread.
struct Sink {
    std::vector<EngineEvent> events;
    void attach(Engine& e)
    {
        // inputs_changed (an input connecting) is not what these tests follow.
        e.onEvent([this](const EngineEvent& ev) {
            if (ev.kind != EventKind::inputs_changed)
                events.push_back(ev);
        });
    }
    size_t count(EventKind k) const
    {
        return static_cast<size_t>(
            std::count_if(events.begin(), events.end(), [k](const EngineEvent& e) { return e.kind == k; }));
    }
    size_t drops(DropReason r) const
    {
        return static_cast<size_t>(std::count_if(events.begin(), events.end(), [r](const EngineEvent& e) {
            return e.kind == EventKind::drop && e.reason && *e.reason == r;
        }));
    }
};

const TrackSnapshot* find(const Snapshot& s, TrackId id)
{
    for (const auto& t : s.tracks)
        if (t.track.id == id)
            return &t;
    return nullptr;
}

TrackId icao(uint32_t a) { return TrackId{a, AddressType::icao}; }

// Lines of the sent JSONL, in order.
std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        out.push_back(line);
    return out;
}

bool has(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

}  // namespace

BOOST_AUTO_TEST_CASE(set_filter_config_drops_outside_tracks_immediately)
{
    TempDir tmp;
    Config cfg = baseConfig();
    cfg.max_range_nm = 100;
    FakeSource* fake = nullptr;
    size_t expected_drops = 0;
    {
        EngineFx fx(cfg, fakeSource(fake), 10s, tmp.path);
        Events ev;
        skycue::Client client(clientConfig(fx.port()));
        ev.attach(client);
        client.start();
        std::this_thread::sleep_for(300ms);
        fx.t.call([&] {
            fake->connection(true);
            for (uint32_t i = 0; i < 10; ++i)  // about 3, 9, ..., 57 nm
                fake->emit(nearbyObservation(0x100000 + i, 0.1 * i));
        });
        BOOST_TEST(ev.waitFor([&] { return ev.ids().size() == 10; }));

        // All in one io-thread call: nothing else runs between the setter and the checks.
        Sink sink;
        size_t fwd_after = 0, drops_now = 0;
        fx.t.call([&] {
            const Snapshot before = fx.engine->snapshot();
            for (const auto& t : before.tracks) {
                BOOST_TEST(t.forwarded);
                expected_drops += t.range_nm > 20 ? 1 : 0;
            }
            sink.attach(*fx.engine);
            fx.engine->setFilterConfig(FilterConfig{20, 0}, "ui");
            drops_now = sink.drops(DropReason::filtered);
            fwd_after = static_cast<size_t>(fx.engine->stats().tracks_forwarded);
            BOOST_TEST(fx.engine->snapshot().filters.max_range_nm == 20.0);
        });
        BOOST_TEST(expected_drops >= 5u);
        BOOST_TEST(drops_now == expected_drops);
        BOOST_TEST(fwd_after == 10u - expected_drops);
        BOOST_TEST(fx.stats().drops_filtered == expected_drops);
        BOOST_TEST(ev.waitFor([&] { return ev.dropsWith(DropReason::filtered) == expected_drops; }));
        fx.stop();
        client.stop();
    }

    const auto sent = lines(streamText(tmp.path, "sent"));
    size_t change_at = sent.size(), filtered_seen = 0;
    bool drop_before_change = false;
    for (size_t i = 0; i < sent.size(); ++i) {
        if (has(sent[i], "\"event\":\"filter_change\"") && has(sent[i], "ui: max_range_nm=20 min_elevation_deg=0"))
            change_at = i;
        if (has(sent[i], "\"event\":\"drop\"") && has(sent[i], "filtered")) {
            ++filtered_seen;
            if (change_at == sent.size())
                drop_before_change = true;
        }
    }
    BOOST_TEST(change_at < sent.size());
    BOOST_TEST(!drop_before_change);  // the filter_change record precedes the drops
    BOOST_TEST(filtered_seen == expected_drops);
    BOOST_TEST(countOf(streamText(tmp.path, "sent"), "\"event\":\"filter_change\"") == 2u);  // startup + ui
}

BOOST_AUTO_TEST_CASE(set_filter_config_resends_newly_eligible)
{
    Config cfg = baseConfig();
    cfg.max_range_nm = 20;
    cfg.min_interval_s = 5;  // the resend must not wait for it
    cfg.max_interval_s = 60;
    FakeSource* fake = nullptr;
    EngineFx fx(cfg, fakeSource(fake));
    Events ev;
    skycue::Client client(clientConfig(fx.port()));
    ev.attach(client);
    client.start();
    std::this_thread::sleep_for(300ms);
    const TrackId never = icao(0x200001), left = icao(0x200002);
    fx.t.call([&] {
        fake->connection(true);
        fake->emit(nearbyObservation(never.address, 0.6));  // 39 nm: never sent
        fake->emit(nearbyObservation(left.address, 0));     // 3 nm: sent
        fake->emit(nearbyObservation(left.address, 0.6));   // 39 nm: dropped (filtered)
    });
    BOOST_TEST(ev.waitFor([&] { return ev.dropped(left, DropReason::filtered); }));

    Sink sink;
    std::set<TrackId> updated;
    fx.t.call([&] {
        sink.attach(*fx.engine);
        fx.engine->setFilterConfig(FilterConfig{100, 0}, "ui");
        for (const auto& e : sink.events)
            if (e.kind == EventKind::update && e.track) {
                updated.insert(e.track->track.id);
                BOOST_TEST(e.track->forwarded);
                BOOST_TEST(!e.track->track.keyframe);
            }
        const Snapshot s = fx.engine->snapshot();
        BOOST_TEST(find(s, never)->forwarded);
        BOOST_TEST(find(s, left)->forwarded);
    });
    BOOST_TEST((updated == std::set<TrackId>{never, left}));
    BOOST_TEST(ev.waitFor([&] { return ev.updatesFor(never) == 1 && ev.updatesFor(left) == 2; }, 1s));
    BOOST_TEST(sink.count(EventKind::filters_changed) == 1u);
    BOOST_TEST(sink.count(EventKind::drop) == 0u);

    // Widening again changes nothing: both are already forwarded.
    const size_t before = fx.t.call([&] { return sink.events.size(); });
    fx.t.call([&] { fx.engine->setFilterConfig(FilterConfig{120, 0}, "ui"); });
    BOOST_TEST(fx.t.call([&] { return sink.events.size(); }) == before + 1);  // filters_changed only

    // A bad value is rejected and changes nothing.
    BOOST_CHECK_THROW(fx.t.call([&] { fx.engine->setFilterConfig(FilterConfig{0, 0}, "ui"); }),
                      std::invalid_argument);
    BOOST_CHECK_THROW(fx.t.call([&] { fx.engine->setFilterConfig(FilterConfig{50, 95}, "ui"); }),
                      std::invalid_argument);
    BOOST_TEST(fx.t.call([&] { return fx.engine->snapshot().filters.max_range_nm; }) == 120.0);
    client.stop();
}

BOOST_AUTO_TEST_CASE(add_block_entry_drops_now)
{
    TempDir tmp;
    Config cfg = baseConfig();
    cfg.min_interval_s = 5;
    cfg.max_interval_s = 60;
    FakeSource* fake = nullptr;
    const TrackId a = icao(0x3a3a3a), b = icao(0x3b3b3b);
    {
        EngineFx fx(cfg, fakeSource(fake), 10s, tmp.path);
        Events ev;
        skycue::Client client(clientConfig(fx.port()));
        ev.attach(client);
        client.start();
        std::this_thread::sleep_for(300ms);
        fx.t.call([&] {
            fake->connection(true);
            fake->emit(nearbyObservation(a.address, 0));
            fake->emit(nearbyObservation(b.address, 0.01));
        });
        BOOST_TEST(ev.waitFor([&] { return ev.ids().size() == 2; }));

        Sink sink;
        fx.t.call([&] {
            sink.attach(*fx.engine);
            fx.engine->addListEntry(ListKind::block, "3A3A3A", "ui");
            BOOST_TEST(sink.drops(DropReason::filtered) == 1u);
            BOOST_TEST((*sink.events.back().id == a));
            BOOST_TEST(sink.count(EventKind::lists_changed) == 1u);
            BOOST_TEST(!fx.engine->snapshot().tracks.empty());
            BOOST_TEST((find(fx.engine->snapshot(), a)->decision == Decision::blocked));
            BOOST_TEST(!find(fx.engine->snapshot(), a)->forwarded);
            BOOST_TEST(find(fx.engine->snapshot(), b)->forwarded);
            BOOST_TEST((fx.engine->blockList().entries() == std::vector<std::string>{"3A3A3A"}));
            BOOST_TEST(fx.engine->snapshot().block_entries == 1u);
        });
        BOOST_TEST(ev.waitFor([&] { return ev.dropped(a, DropReason::filtered); }));

        // Adding the same entry again (any case) is a no-op: no record, no event.
        fx.t.call([&] {
            const size_t n = sink.events.size();
            fx.engine->addListEntry(ListKind::block, "3a3a3a", "ui");
            BOOST_TEST(sink.events.size() == n);
        });
        BOOST_CHECK_THROW(fx.t.call([&] { fx.engine->addListEntry(ListKind::block, "not an id!", "ui"); }),
                          std::invalid_argument);

        // Removing it resends the track at once, despite min_interval_s.
        fx.t.call([&] {
            sink.events.clear();
            fx.engine->removeListEntry(ListKind::block, "3a3a3a", "ui");
            BOOST_TEST(sink.count(EventKind::lists_changed) == 1u);
            BOOST_TEST(sink.count(EventKind::update) == 1u);
            BOOST_TEST(fx.engine->blockList().entries().empty());
            BOOST_TEST(find(fx.engine->snapshot(), a)->forwarded);
        });
        BOOST_TEST(ev.waitFor([&] { return ev.updatesFor(a) == 2; }, 1s));

        // Allow-listing an out-of-range track sends it; replacing the lists drops it again.
        const TrackId far = icao(0x3c3c3c);
        fx.t.call([&] {
            fake->emit(nearbyObservation(far.address, 4.0));  // about 243 nm
            BOOST_TEST(!find(fx.engine->snapshot(), far)->forwarded);
            fx.engine->addListEntry(ListKind::allow, "3c3c3c", "ui");
            BOOST_TEST((find(fx.engine->snapshot(), far)->decision == Decision::allow_listed));
            BOOST_TEST(find(fx.engine->snapshot(), far)->forwarded);
            fx.engine->setLists(skycued::IdList(), skycued::IdList::parse("3b3b3b\n"), "ui");
            BOOST_TEST(!find(fx.engine->snapshot(), far)->forwarded);
            BOOST_TEST(!find(fx.engine->snapshot(), b)->forwarded);
            BOOST_TEST(find(fx.engine->snapshot(), a)->forwarded);
            BOOST_TEST(fx.engine->allowList().size() == 0u);
        });
        fx.stop();
        client.stop();
    }
    const std::string sent = streamText(tmp.path, "sent");
    BOOST_TEST(countOf(sent, "ui: block += 3A3A3A") == 1u);
    BOOST_TEST(countOf(sent, "ui: block -= 3a3a3a") == 1u);
    BOOST_TEST(countOf(sent, "ui: allow += 3c3c3c") == 1u);
    BOOST_TEST(countOf(sent, "ui: allow_list=0 entries block_list=1 entries") == 1u);
    BOOST_TEST(countOf(sent, "\"event\":\"filter_change\"") == 5u);  // startup + 4 changes
}

BOOST_AUTO_TEST_CASE(snapshot_matches_table)
{
    Config cfg = baseConfig();
    cfg.max_range_nm = 20;
    FakeSource* fake = nullptr;
    EngineFx fx(cfg, fakeSource(fake));
    const TrackId in = icao(0x400001), out = icao(0x400002), blocked = icao(0x400003), nopos = icao(0x400004);
    const auto t0 = std::chrono::system_clock::now() - 2s;
    fx.t.call([&] {
        fx.engine->addListEntry(ListKind::block, "400003", "test");
        fake->connection(true);
        TrackObservation o = nearbyObservation(in.address, 0);
        o.received_time = t0;
        o.position_time = t0;
        o.emergency = "none";
        o.category = "A3";
        o.rssi = -12.5;
        fake->emit(o);
        o.received_time = t0 + 1s;
        o.position_time = t0 + 1s;
        fake->emit(o);
        fake->emit(nearbyObservation(out.address, 0.6));
        fake->emit(nearbyObservation(blocked.address, 0.01));
        TrackObservation n = nearbyObservation(nopos.address, 0);
        n.lat.reset();
        n.lon.reset();
        n.position_time.reset();
        n.emergency = "general";
        n.category = "A1";
        fake->emit(n);
    });
    const Snapshot s = fx.t.call([&] { return fx.engine->snapshot(); });
    BOOST_TEST(s.tracks.size() == 4u);
    BOOST_TEST(s.filters.max_range_nm == 20.0);
    BOOST_TEST(s.filters.min_elevation_deg == 0.0);
    BOOST_TEST(s.allow_entries == 0u);
    BOOST_TEST(s.block_entries == 1u);
    BOOST_TEST(s.stats.tracks_total == 4u);
    BOOST_TEST(s.stats.tracks_forwarded == 1u);
    BOOST_TEST(s.stats.observations == 5u);
    BOOST_TEST(s.stats.sent_updates == 1u);
    BOOST_TEST(s.stats.tcp_clients == 0u);
    BOOST_TEST(s.stats.udp_targets == 0u);
    BOOST_TEST(s.stats.web_clients == 0u);

    const TrackSnapshot* ti = find(s, in);
    BOOST_REQUIRE(ti);
    BOOST_TEST((ti->decision == Decision::send));
    BOOST_TEST(ti->forwarded);
    BOOST_TEST(ti->range_nm > 2.5);
    BOOST_TEST(ti->range_nm < 3.5);
    BOOST_TEST(std::isfinite(ti->elevation_deg));
    BOOST_TEST((ti->first_seen == t0));
    BOOST_TEST((ti->last_seen == t0 + 1s));
    BOOST_TEST(ti->emergency.value_or("") == "none");
    BOOST_TEST(ti->category.value_or("") == "A3");
    BOOST_TEST(ti->rssi == -12.5);
    BOOST_TEST(ti->track.lat_deg == 38.95);

    const TrackSnapshot* to = find(s, out);
    BOOST_REQUIRE(to);
    BOOST_TEST((to->decision == Decision::out_of_range));
    BOOST_TEST(!to->forwarded);
    BOOST_TEST(to->range_nm > 20.0);
    BOOST_TEST(std::isnan(to->rssi));
    BOOST_TEST(!to->emergency);

    const TrackSnapshot* tb = find(s, blocked);
    BOOST_REQUIRE(tb);
    BOOST_TEST((tb->decision == Decision::blocked));
    BOOST_TEST(!tb->forwarded);
    BOOST_TEST(std::isfinite(tb->range_nm));

    const TrackSnapshot* tn = find(s, nopos);
    BOOST_REQUIRE(tn);
    BOOST_TEST((tn->decision == Decision::no_position));
    BOOST_TEST(!tn->forwarded);
    BOOST_TEST(std::isnan(tn->range_nm));
    BOOST_TEST(std::isnan(tn->elevation_deg));
    BOOST_TEST(tn->emergency.value_or("") == "general");
    BOOST_TEST(tn->category.value_or("") == "A1");
    BOOST_TEST((tn->first_seen == tn->last_seen));

    // Taking a snapshot changes nothing.
    const Snapshot again = fx.t.call([&] { return fx.engine->snapshot(); });
    BOOST_TEST(again.tracks.size() == 4u);
    BOOST_TEST(again.stats.sent_updates == 1u);
    BOOST_TEST(fx.t.call([&] { return fx.engine->stats().sent_drops; }) == 0u);
}

BOOST_AUTO_TEST_CASE(on_event_sequence)
{
    Config cfg = baseConfig();
    cfg.max_range_nm = 100;
    FakeSource* fake = nullptr;
    EngineFx fx(cfg, fakeSource(fake));
    Sink sink;
    const TrackId id = icao(0x500001);
    fx.t.call([&] {
        sink.attach(*fx.engine);
        fx.engine->setWebClientCount(3);
        fake->connection(true);
        fake->emit(nearbyObservation(id.address, 0.6));  // 39 nm: sent
        fx.engine->setFilterConfig(FilterConfig{20, 0}, "ui");  // dropped
        fx.engine->addListEntry(ListKind::allow, "N512RG", "ui");
    });
    std::vector<EventKind> kinds = fx.t.call([&] {
        std::vector<EventKind> k;
        for (const auto& e : sink.events)
            k.push_back(e.kind);
        return k;
    });
    const std::vector<EventKind> head{EventKind::update, EventKind::observed, EventKind::filters_changed,
                                      EventKind::drop, EventKind::lists_changed};
    BOOST_REQUIRE(kinds.size() >= head.size());
    BOOST_TEST((std::vector<EventKind>(kinds.begin(), kinds.begin() + 5) == head));
    fx.t.call([&] {
        const EngineEvent& up = sink.events[0];
        BOOST_REQUIRE(up.track);
        BOOST_TEST((up.track->track.id == id));
        BOOST_TEST(up.track->forwarded);
        BOOST_TEST((up.track->decision == Decision::send));
        const EngineEvent& ob = sink.events[1];  // after the update the observation caused
        BOOST_REQUIRE(ob.track);
        BOOST_TEST((ob.track->track.id == id));
        BOOST_TEST(ob.track->forwarded);
        BOOST_TEST(ob.track->range_nm > 30.0);
        const EngineEvent& dr = sink.events[3];
        BOOST_TEST((dr.id.value_or(TrackId{}) == id));
        BOOST_TEST((dr.reason.value_or(DropReason::shutdown) == DropReason::filtered));
        BOOST_TEST(!dr.track);
    });

    // The 1 s tick: a stats event carrying the counters.
    std::this_thread::sleep_for(1300ms);
    fx.t.call([&] {
        BOOST_TEST(sink.count(EventKind::stats) >= 1u);
        const auto it = std::find_if(sink.events.begin(), sink.events.end(),
                                     [](const EngineEvent& e) { return e.kind == EventKind::stats; });
        BOOST_REQUIRE(it->stats);
        BOOST_TEST(it->stats->web_clients == 3u);
        BOOST_TEST(it->stats->drops_filtered == 1u);
        BOOST_TEST(it->stats->sent_updates == 1u);
        BOOST_TEST(it->stats->tracks_total == 1u);
    });

    // A timeout drop is an event too (track_timeout_s = 5 on the data clock).
    const TrackId stale = icao(0x500002);
    fx.t.call([&] {
        TrackObservation o = nearbyObservation(stale.address, 0);
        o.received_time -= 10s;
        o.position_time = o.received_time;
        fake->emit(o);  // sent, then expires on the next tick
        fake->emit(nearbyObservation(id.address, 0.6));  // moves the data clock to now; still outside
    });
    std::this_thread::sleep_for(1300ms);
    fx.t.call([&] {
        BOOST_TEST(sink.drops(DropReason::timeout) == 1u);
        BOOST_TEST(fx.engine->stats().drops_timeout == 1u);
    });

    // Nothing after stop(), not even for the shutdown drops.
    const size_t n = fx.t.call([&] {
        fake->emit(nearbyObservation(0x500003, 0));  // forwarded at shutdown
        const size_t c = sink.events.size();
        fx.engine->stop();
        BOOST_TEST(sink.events.size() == c);
        fx.engine->setFilterConfig(FilterConfig{50, 0}, "ui");  // ignored after stop()
        return sink.events.size();
    });
    std::this_thread::sleep_for(1300ms);
    BOOST_TEST(fx.t.call([&] { return sink.events.size(); }) == n);
}

BOOST_AUTO_TEST_CASE(expired_event_for_unforwarded_track)
{
    Config cfg = baseConfig();
    cfg.max_range_nm = 20;
    FakeSource* fake = nullptr;
    EngineFx fx(cfg, fakeSource(fake));
    Sink sink;
    const TrackId far = icao(0x600001), near = icao(0x600002), live = icao(0x600003);
    fx.t.call([&] {
        sink.attach(*fx.engine);
        fake->connection(true);
        // Both heard 10 s ago (cues.track_timeout_s is 5): they expire on the next tick.
        TrackObservation f = nearbyObservation(far.address, 0.6);  // 39 nm: out of range, never sent
        f.received_time -= 10s;
        f.position_time = f.received_time;
        fake->emit(f);
        TrackObservation n = nearbyObservation(near.address, 0);  // sent
        n.received_time -= 10s;
        n.position_time = n.received_time;
        fake->emit(n);
        fake->emit(nearbyObservation(live.address, 0.01));  // moves the data clock to now
    });
    std::this_thread::sleep_for(1300ms);
    fx.t.call([&] {
        std::vector<EngineEvent> expired, drops;
        for (const auto& e : sink.events) {
            if (e.kind == EventKind::expired)
                expired.push_back(e);
            if (e.kind == EventKind::drop)
                drops.push_back(e);
        }
        // The never-forwarded track: an expired event, no drop.
        BOOST_REQUIRE(expired.size() == 1u);
        BOOST_TEST((expired[0].id.value_or(TrackId{}) == far));
        BOOST_TEST(!expired[0].reason);
        BOOST_TEST(!expired[0].track);
        // The forwarded one: a timeout drop, as before, and no expired event.
        BOOST_REQUIRE(drops.size() == 1u);
        BOOST_TEST((drops[0].id.value_or(TrackId{}) == near));
        BOOST_TEST((drops[0].reason.value_or(DropReason::shutdown) == DropReason::timeout));
        BOOST_TEST(fx.engine->snapshot().tracks.size() == 1u);
        BOOST_TEST(fx.engine->stats().sent_drops == 1u);
    });
}

// ---------------------------------------------------------------- /api/inputs

// Compare optionals outside BOOST_TEST's expression decomposition: Boost 1.66
// cannot resolve operator== between value_expr<std::optional<T>> and optional.
template <class T>
void expectOpt(const std::optional<T>& got, const typename std::remove_cv<T>::type& want, const char* what) {
    BOOST_TEST_CONTEXT(what) {
        BOOST_TEST(got.has_value());
        if (got) BOOST_TEST(*got == want);
    }
}

namespace {

using skycued::JsonObject;
using skycued::webapi::ApiRequest;
using skycued::webapi::ApiResponse;
using Headers = std::map<std::string, std::string>;

std::string psJson(const std::string& id, uint16_t port, const std::string& extra = "")
{
    std::string body = "{";
    if (!id.empty())
        body += R"("id":")" + id + R"(",)";
    return body + R"("type":"pingstation_json","bind":"127.0.0.1","port":)" + std::to_string(port) + extra + "}";
}

JsonObject parsed(const std::string& text)
{
    auto o = JsonObject::parse(text);
    if (!o)
        throw std::runtime_error("not a JSON object: " + text);
    return *o;
}

// The /api/inputs list's entry with this id; nullopt when there is none.
std::optional<JsonObject> listed(const std::string& body, const std::string& id)
{
    const auto list = parsed(body).getObjectArray("inputs");
    if (!list)
        throw std::runtime_error("no inputs array: " + body);
    for (const auto& in : *list)
        if (in.getString("id") == id)
            return in;
    return std::nullopt;
}

// An Engine over a FakeSource built-in "readsb", its InputManager backed by
// <tmp>/skycue.inputs.json, and a RuntimeConfig, all on the io thread;
// call() answers one request through handleApi there. With `replaying` the
// built-in is "replay" and the manager is in replay mode.
struct ApiFx {
    TempDir tmp;
    std::string conf, inputs_file;
    FakeSource* src = nullptr;
    std::unique_ptr<EngineFx> fx;
    std::unique_ptr<skycued::RuntimeConfig> rc;

    explicit ApiFx(bool replaying = false)
    {
        conf = tmp.path + "/skycue.toml";
        inputs_file = tmp.path + "/skycue.inputs.json";
        std::ofstream(conf) << skycue_test::tomlConf({{"receiver.lat", "38.9"}, {"receiver.lon", "-77.0"}, {"cues.track_timeout_s", "5"}});
        std::function<std::unique_ptr<InputManager>(asio::io_context&)> make = [this, replaying](
                                                                                   asio::io_context& io) {
            auto s = std::make_unique<FakeSource>();
            src = s.get();
            if (!replaying)
                return sourceInputs(io, std::move(s), "readsb", inputs_file);
            skycued::InputManagerConfig mc;
            mc.inputs_file = inputs_file;
            mc.replaying = true;
            auto m = std::make_unique<InputManager>(io, mc);
            InputDef def;
            def.id = "replay";
            def.type = InputType::replay;
            m->addBuiltin(std::make_unique<skycued::SourceInput>(def, std::move(s)));
            m->load();
            return m;
        };
        fx = std::make_unique<EngineFx>(skycued::parseConfigFile(conf), make);
        fx->t.call([&] {
            rc = std::make_unique<skycued::RuntimeConfig>(fx->t.io, conf, *fx->engine);
            rc->load();
        });
    }
    ~ApiFx()
    {
        try {
            fx->t.call([&] { rc.reset(); });
        } catch (...) {
        }
    }

    ApiResponse call(const std::string& method, const std::string& path, const std::string& body, Headers headers)
    {
        ApiRequest req;
        req.method = method;
        req.path = path;
        req.headers = std::move(headers);
        req.body = body;
        return fx->t.call([&] {
            skycued::webapi::ApiContext ctx{*fx->engine, *rc, tmp.path};
            return skycued::webapi::handleApi(req, ctx);
        });
    }
    // With Host, and the JSON content type on PUT and POST.
    ApiResponse call(const std::string& method, const std::string& path, const std::string& body = "")
    {
        Headers h{{"host", "127.0.0.1:8080"}};
        if (method == "PUT" || method == "POST")
            h["content-type"] = "application/json";
        return call(method, path, body, h);
    }
    ApiResponse get(const std::string& path) { return call("GET", path); }
};

}  // namespace

BOOST_AUTO_TEST_CASE(inputs_get_lists_builtins)
{
    ApiFx a;
    a.fx->t.call([&] { a.src->connection(true); });
    const auto r = a.get("/api/inputs");
    BOOST_TEST(r.status == 200u);
    const auto readsb = listed(r.body, "readsb");
    BOOST_REQUIRE_MESSAGE(readsb, r.body);
    expectOpt(readsb->getBool("builtin"), true, "readsb->getBool('builtin')");
    expectOpt(readsb->getString("type"), "readsb", "readsb->getString('type')");
    const auto state = readsb->getObject("state");
    BOOST_REQUIRE_MESSAGE(state, r.body);
    expectOpt(state->getBool("connected"), true, "state->getBool('connected')");
    BOOST_TEST(state->getBool("receiving").has_value());
    for (const char* k : {"peers", "status_fail_streak", "connects", "bytes", "messages", "observations", "uplinks",
                          "rejected", "records_skipped", "sac_sic_mismatch", "nogo_records", "clock_rejects",
                          "rejected_peer", "raw_unknown_len", "status_ok", "status_fail", "capture_dropped"})
        BOOST_TEST(state->getInt(k).has_value(), k);
    expectOpt(state->getString("last_error"), "", "state->getString('last_error')");
    BOOST_TEST(!state->has("device_status"));
    BOOST_TEST(!state->has("last_data_age_s"));  // nothing arrived yet
    a.fx->t.call([&] { a.src->emit(nearbyObservation(0xABC001, 0)); });
    const auto after = listed(a.get("/api/inputs").body, "readsb");
    expectOpt(after->getObject("state")->getInt("observations"), 1, "after->getObject('state')->getInt('observations')");
}

BOOST_AUTO_TEST_CASE(inputs_post_creates_201)
{
    ApiFx a;
    const uint16_t port = freeUdpPort();
    const auto r = a.call("POST", "/api/inputs", psJson("ps-a", port, R"(,"name":"North")"));
    BOOST_TEST(r.status == 201u, r.body);
    const auto in = parsed(r.body);
    expectOpt(in.getString("id"), "ps-a", "in.getString('id')");
    expectOpt(in.getString("name"), "North", "in.getString('name')");
    expectOpt(in.getInt("port"), port, "in.getInt('port')");
    expectOpt(in.getBool("builtin"), false, "in.getBool('builtin')");
    BOOST_TEST(in.getObject("state").has_value());
    const auto list = a.get("/api/inputs").body;
    BOOST_TEST(listed(list, "ps-a").has_value());
    BOOST_TEST(has(readAll(a.inputs_file), "\"ps-a\""));
}

BOOST_AUTO_TEST_CASE(inputs_post_missing_id_400)
{
    ApiFx a;
    const auto r = a.call("POST", "/api/inputs", psJson("", freeUdpPort()));
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(r.body == R"({"error":"id is required"})");
    BOOST_TEST(a.call("POST", "/api/inputs", "not json").status == 400u);
}

BOOST_AUTO_TEST_CASE(inputs_post_unknown_key_400_names_key)
{
    ApiFx a;
    const uint16_t port = freeUdpPort();
    const auto r = a.call("POST", "/api/inputs", psJson("ps-a", port, R"(,"colour":"red")"));
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(has(r.body, "'colour'"), r.body);
    // builtin and state are output-only.
    for (const char* extra : {R"(,"builtin":true)", R"(,"state":{})"}) {
        const auto o = a.call("POST", "/api/inputs", psJson("ps-a", port, extra));
        BOOST_TEST(o.status == 400u, o.body);
    }
    BOOST_TEST(!listed(a.get("/api/inputs").body, "ps-a"));
}

BOOST_AUTO_TEST_CASE(inputs_post_duplicate_409)
{
    ApiFx a;
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-a", freeUdpPort())).status == 201u);
    const auto r = a.call("POST", "/api/inputs", psJson("ps-a", freeUdpPort()));
    BOOST_TEST(r.status == 409u, r.body);
    // A built-in's id is reserved: a user input cannot take it.
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("readsb", freeUdpPort())).status == 400u);
}

BOOST_AUTO_TEST_CASE(inputs_post_port_clash_400)
{
    ApiFx a;
    const uint16_t port = freeUdpPort();
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-a", port)).status == 201u);
    const auto r = a.call("POST", "/api/inputs", psJson("ps-b", port));
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(has(r.body, "ps-a"), r.body);
}

BOOST_AUTO_TEST_CASE(inputs_put_replaces)
{
    ApiFx a;
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-a", freeUdpPort())).status == 201u);
    const uint16_t port2 = freeUdpPort();
    // Without an id the path's applies; with one it must be the same.
    auto r = a.call("PUT", "/api/inputs/ps-a", psJson("", port2, R"(,"name":"South")"));
    BOOST_TEST(r.status == 200u, r.body);
    expectOpt(parsed(r.body).getString("name"), "South", "parsed(r.body).getString('name')");
    expectOpt(parsed(r.body).getInt("port"), port2, "parsed(r.body).getInt('port')");
    r = a.call("PUT", "/api/inputs/ps-a", psJson("ps-a", port2, R"(,"name":"East")"));
    BOOST_TEST(r.status == 200u, r.body);
    const auto in = listed(a.get("/api/inputs").body, "ps-a");
    expectOpt(in->getString("name"), "East", "in->getString('name')");
    BOOST_TEST(has(readAll(a.inputs_file), "East"));
    // A bad definition is a 400 with the parser's message.
    r = a.call("PUT", "/api/inputs/ps-a", psJson("", 0));
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(has(r.body, "port"), r.body);
}

BOOST_AUTO_TEST_CASE(inputs_put_id_mismatch_400)
{
    ApiFx a;
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-a", freeUdpPort())).status == 201u);
    const auto r = a.call("PUT", "/api/inputs/ps-a", psJson("ps-b", freeUdpPort()));
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(has(r.body, "ps-b"), r.body);
    BOOST_TEST(listed(a.get("/api/inputs").body, "ps-a").has_value());
}

BOOST_AUTO_TEST_CASE(inputs_put_unknown_404)
{
    ApiFx a;
    BOOST_TEST(a.call("PUT", "/api/inputs/nope", psJson("", freeUdpPort())).status == 404u);
    BOOST_TEST(a.call("PUT", "/api/inputs/nope", psJson("nope", freeUdpPort())).status == 404u);
}

BOOST_AUTO_TEST_CASE(inputs_put_builtin_only_enabled)
{
    ApiFx a;
    auto r = a.call("PUT", "/api/inputs/readsb", R"({"enabled":false})");
    BOOST_TEST(r.status == 200u, r.body);
    expectOpt(parsed(r.body).getBool("enabled"), false, "parsed(r.body).getBool('enabled')");
    expectOpt(listed(a.get("/api/inputs").body, "readsb")->getBool("enabled"), false, "listed(a.get('/api/inputs').body, 'readsb')->getBool('enabled')");
    r = a.call("PUT", "/api/inputs/readsb", R"({"enabled":false,"port":1})");
    BOOST_TEST(r.status == 403u, r.body);
    r = a.call("PUT", "/api/inputs/readsb", R"({"enabled":"maybe"})");
    BOOST_TEST(r.status == 400u, r.body);
    r = a.call("PUT", "/api/inputs/readsb", R"({"enabled":true})");
    BOOST_TEST(r.status == 200u, r.body);
    expectOpt(listed(a.get("/api/inputs").body, "readsb")->getBool("enabled"), true, "listed(a.get('/api/inputs').body, 'readsb')->getBool('enabled')");
}

BOOST_AUTO_TEST_CASE(inputs_delete_204_then_404)
{
    ApiFx a;
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-a", freeUdpPort())).status == 201u);
    auto r = a.call("DELETE", "/api/inputs/ps-a");
    BOOST_TEST(r.status == 204u);
    BOOST_TEST(r.body.empty());
    BOOST_TEST(!listed(a.get("/api/inputs").body, "ps-a"));
    BOOST_TEST(!has(readAll(a.inputs_file), "ps-a"));
    r = a.call("DELETE", "/api/inputs/ps-a");
    BOOST_TEST(r.status == 404u);
}

BOOST_AUTO_TEST_CASE(inputs_delete_builtin_403)
{
    ApiFx a;
    BOOST_TEST(a.call("DELETE", "/api/inputs/readsb").status == 403u);
    BOOST_TEST(listed(a.get("/api/inputs").body, "readsb").has_value());
}

BOOST_AUTO_TEST_CASE(inputs_delete_cross_origin_403)
{
    ApiFx a;
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-a", freeUdpPort())).status == 201u);
    const auto r =
        a.call("DELETE", "/api/inputs/ps-a", "", Headers{{"host", "127.0.0.1:8080"}, {"origin", "http://evil.example"}});
    BOOST_TEST(r.status == 403u, r.body);
    BOOST_TEST(listed(a.get("/api/inputs").body, "ps-a").has_value());
}

BOOST_AUTO_TEST_CASE(inputs_delete_without_content_type_ok)
{
    ApiFx a;
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-a", freeUdpPort())).status == 201u);
    const auto r =
        a.call("DELETE", "/api/inputs/ps-a", "", Headers{{"host", "127.0.0.1:8080"}, {"origin", "http://127.0.0.1:8080"}});
    BOOST_TEST(r.status == 204u, r.body);
}

BOOST_AUTO_TEST_CASE(inputs_enable_disable)
{
    ApiFx a;
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-a", freeUdpPort())).status == 201u);
    auto r = a.call("POST", "/api/inputs/ps-a/disable", "{}");
    BOOST_TEST(r.status == 200u, r.body);
    expectOpt(parsed(r.body).getBool("enabled"), false, "parsed(r.body).getBool('enabled')");
    expectOpt(parsed(r.body).getObject("state")->getBool("connected"), false, "parsed(r.body).getObject('state')->getBool('connected')");
    r = a.call("POST", "/api/inputs/ps-a/enable", "{}");
    BOOST_TEST(r.status == 200u, r.body);
    expectOpt(parsed(r.body).getBool("enabled"), true, "parsed(r.body).getBool('enabled')");
    // Built-ins too; the body must be an empty object.
    BOOST_TEST(a.call("POST", "/api/inputs/readsb/disable", "{}").status == 200u);
    expectOpt(listed(a.get("/api/inputs").body, "readsb")->getBool("enabled"), false, "listed(a.get('/api/inputs').body, 'readsb')->getBool('enabled')");
    BOOST_TEST(a.call("POST", "/api/inputs/readsb/enable", R"({"x":1})").status == 400u);
    BOOST_TEST(a.call("POST", "/api/inputs/nope/enable", "{}").status == 404u);
}

BOOST_AUTO_TEST_CASE(inputs_status_route)
{
    ApiFx a;
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-a", freeUdpPort(), R"(,"status_interval_s":60)")).status ==
               201u);
    const auto r = a.get("/api/inputs/ps-a/status");
    BOOST_TEST(r.status == 200u, r.body);
    const auto s = parsed(r.body);
    expectOpt(s.getString("id"), "ps-a", "s.getString('id')");
    expectOpt(s.getString("status_url"), "", "s.getString('status_url')");
    expectOpt(s.getInt("status_interval_s"), 60, "s.getInt('status_interval_s')");
    expectOpt(s.getInt("status_ok"), 0, "s.getInt('status_ok')");
    expectOpt(s.getInt("status_fail"), 0, "s.getInt('status_fail')");
    expectOpt(s.getInt("status_fail_streak"), 0, "s.getInt('status_fail_streak')");
    BOOST_TEST(!s.has("device_status"));
    BOOST_TEST(!s.has("state"));
    BOOST_TEST(a.get("/api/inputs/nope/status").status == 404u);
}

BOOST_AUTO_TEST_CASE(inputs_post_wrong_content_type_415)
{
    ApiFx a;
    const auto body = psJson("ps-a", freeUdpPort());
    auto r = a.call("POST", "/api/inputs", body, Headers{{"host", "127.0.0.1:8080"}, {"content-type", "text/plain"}});
    BOOST_TEST(r.status == 415u);
    r = a.call("PUT", "/api/inputs/readsb", R"({"enabled":false})", Headers{{"host", "127.0.0.1:8080"}});
    BOOST_TEST(r.status == 415u);
    r = a.call("POST", "/api/inputs", body,
               Headers{{"host", "127.0.0.1:8080"}, {"content-type", "application/json"}, {"origin", "null"}});
    BOOST_TEST(r.status == 403u);
    BOOST_TEST(!listed(a.get("/api/inputs").body, "ps-a"));
}

BOOST_AUTO_TEST_CASE(inputs_methods_405)
{
    ApiFx a;
    const std::vector<std::tuple<std::string, std::string, std::string>> cases{
        {"DELETE", "/api/inputs", "GET, POST"},
        {"PUT", "/api/inputs", "GET, POST"},
        {"GET", "/api/inputs/readsb", "PUT, DELETE"},
        {"POST", "/api/inputs/readsb", "PUT, DELETE"},
        {"GET", "/api/inputs/readsb/enable", "POST"},
        {"PUT", "/api/inputs/readsb/disable", "POST"},
        {"POST", "/api/inputs/readsb/status", "GET"},
    };
    for (const auto& c : cases) {
        const auto r = a.call(std::get<0>(c), std::get<1>(c), "{}");
        BOOST_TEST(r.status == 405u, std::get<0>(c) << " " << std::get<1>(c));
        BOOST_TEST(r.allow == std::get<2>(c), std::get<0>(c) << " " << std::get<1>(c));
    }
    BOOST_TEST(a.get("/api/inputs/readsb/bogus").status == 404u);
    BOOST_TEST(a.get("/api/inputs/").status == 404u);
}

BOOST_AUTO_TEST_CASE(stats_carry_inputs)
{
    ApiFx a;
    a.fx->t.call([&] {
        a.src->connection(true);
        a.src->emit(nearbyObservation(0xABC001, 0));
    });
    const auto r = a.get("/api/stats");
    BOOST_TEST(r.status == 200u);
    const auto inputs = parsed(r.body).getObjectArray("inputs");
    BOOST_REQUIRE_MESSAGE(inputs, r.body);
    BOOST_REQUIRE_EQUAL(inputs->size(), 1u);
    const auto& in = (*inputs)[0];
    expectOpt(in.getString("id"), "readsb", "in.getString('id')");
    expectOpt(in.getBool("connected"), true, "in.getBool('connected')");
    expectOpt(in.getInt("observations"), 1, "in.getInt('observations')");
    for (const char* k : {"messages", "uplinks", "rejected"})
        BOOST_TEST(in.getInt(k).has_value(), k);
    BOOST_TEST(in.keys().size() == 6u, r.body);
    // The derived built-in flags stay for one release.
    expectOpt(parsed(r.body).getBool("readsb_connected"), true, "parsed(r.body).getBool('readsb_connected')");
}

BOOST_AUTO_TEST_CASE(state_carries_inputs)
{
    ApiFx a;
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-a", freeUdpPort())).status == 201u);
    const auto r = a.get("/api/state");
    BOOST_TEST(r.status == 200u);
    const auto inputs = parsed(r.body).getObjectArray("inputs");
    BOOST_REQUIRE_MESSAGE(inputs, r.body);
    BOOST_REQUIRE_EQUAL(inputs->size(), 2u);
    expectOpt((*inputs)[0].getString("id"), "readsb", "(*inputs)[0].getString('id')");
    expectOpt((*inputs)[1].getString("id"), "ps-a", "(*inputs)[1].getString('id')");
    BOOST_TEST((*inputs)[1].getObject("state").has_value());
}

BOOST_AUTO_TEST_CASE(fisb_status_has_uplink_duplicates)
{
    ApiFx a;
    const auto r = a.get("/api/fisb/status");
    BOOST_TEST(r.status == 200u);
    expectOpt(parsed(r.body).getInt("uplink_duplicates"), 0, "parsed(r.body).getInt('uplink_duplicates')");
}

BOOST_AUTO_TEST_CASE(inputs_path_id_invalid_400)
{
    ApiFx a;
    const std::string longer(7000, 'a');
    for (const std::string& bad : {std::string("Bad_Id"), std::string("a b"), longer}) {
        const std::vector<std::pair<std::string, std::string>> routes{
            {"PUT", "/api/inputs/" + bad},
            {"DELETE", "/api/inputs/" + bad},
            {"POST", "/api/inputs/" + bad + "/enable"},
            {"GET", "/api/inputs/" + bad + "/status"},
        };
        for (const auto& rt : routes) {
            const auto r = a.call(rt.first, rt.second, R"({"enabled":true})");
            BOOST_TEST(r.status == 400u, rt.first << " " << bad.substr(0, 10));
            BOOST_TEST(!has(r.body, bad), r.body);
            BOOST_TEST(r.body.size() < 100u);
        }
    }
}

BOOST_AUTO_TEST_CASE(replay_writes_409)
{
    ApiFx a(true);
    const uint16_t port = freeUdpPort();
    const std::vector<std::tuple<std::string, std::string, std::string>> writes{
        {"POST", "/api/inputs", psJson("ps-a", port)},
        {"POST", "/api/inputs", "not json"},           // 409 before the body is read
        {"PUT", "/api/inputs/nope", psJson("", port)},  // 409 before the lookup
        {"PUT", "/api/inputs/replay", R"({"enabled":false})"},
        {"DELETE", "/api/inputs/replay", ""},
        {"DELETE", "/api/inputs/nope", ""},
        {"POST", "/api/inputs/replay/disable", "{}"},
        {"POST", "/api/inputs/replay/enable", "{}"},
    };
    for (const auto& w : writes) {
        const auto r = a.call(std::get<0>(w), std::get<1>(w), std::get<2>(w));
        BOOST_TEST(r.status == 409u, std::get<0>(w) << " " << std::get<1>(w) << ": " << r.body);
    }
    // Reads still work, and nothing changed.
    const auto list = a.get("/api/inputs");
    BOOST_TEST(list.status == 200u);
    expectOpt(listed(list.body, "replay")->getBool("enabled"), true, "listed(list.body, 'replay')->getBool('enabled')");
    BOOST_TEST(!listed(list.body, "ps-a"));
    BOOST_TEST(a.get("/api/inputs/replay/status").status == 200u);
}

BOOST_AUTO_TEST_CASE(inputs_error_messages_clipped)
{
    ApiFx a;
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-a", freeUdpPort())).status == 201u);
    const std::string longer(5000, 'b');
    auto r = a.call("PUT", "/api/inputs/ps-a", psJson(longer, freeUdpPort()));
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(!has(r.body, longer), r.body.size());
    BOOST_TEST(has(r.body, std::string(64, 'b') + "..."), r.body);
    BOOST_TEST(r.body.size() < 200u);
    r = a.call("POST", "/api/inputs", psJson("ps-b", freeUdpPort(), ",\"" + longer + "\":1"));
    BOOST_TEST(r.status == 400u);
    BOOST_TEST(has(r.body, "unknown key '" + std::string(64, 'b') + "...'"), r.body);
    r = a.get("/api/inputs/ps-a/" + longer);
    BOOST_TEST(r.status == 404u);
    BOOST_TEST(r.body.size() < 200u);
}

BOOST_AUTO_TEST_CASE(inputs_writes_cross_origin_403)
{
    ApiFx a;
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-a", freeUdpPort())).status == 201u);
    const Headers foreign{
        {"host", "127.0.0.1:8080"}, {"content-type", "application/json"}, {"origin", "http://evil.example"}};
    BOOST_TEST(a.call("PUT", "/api/inputs/ps-a", psJson("", freeUdpPort(), R"(,"name":"Evil")"), foreign).status ==
               403u);
    BOOST_TEST(a.call("PUT", "/api/inputs/readsb", R"({"enabled":false})", foreign).status == 403u);
    BOOST_TEST(a.call("POST", "/api/inputs/ps-a/disable", "{}", foreign).status == 403u);
    BOOST_TEST(a.call("POST", "/api/inputs/readsb/enable", "{}", foreign).status == 403u);
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-b", freeUdpPort()), foreign).status == 403u);
    const auto list = a.get("/api/inputs").body;
    expectOpt(listed(list, "ps-a")->getString("name"), "", "listed(list, 'ps-a')->getString('name')");
    expectOpt(listed(list, "ps-a")->getBool("enabled"), true, "listed(list, 'ps-a')->getBool('enabled')");
    expectOpt(listed(list, "readsb")->getBool("enabled"), true, "listed(list, 'readsb')->getBool('enabled')");
    BOOST_TEST(!listed(list, "ps-b"));
    // Without an Origin header (curl, scripts) a write is allowed, as for the
    // other routes: browsers always send one on these requests.
    const Headers none{{"host", "127.0.0.1:8080"}, {"content-type", "application/json"}};
    BOOST_TEST(a.call("POST", "/api/inputs", psJson("ps-b", freeUdpPort()), none).status == 201u);
    BOOST_TEST(a.call("POST", "/api/inputs/ps-b/disable", "{}", none).status == 200u);
    BOOST_TEST(a.call("DELETE", "/api/inputs/ps-b", "", Headers{{"host", "127.0.0.1:8080"}}).status == 204u);
}

// The live server (web_server.cpp) builds its ApiRequest with this, so the
// fuzz_web harness drives handleApi exactly as a socket would.
BOOST_AUTO_TEST_CASE(make_api_request_splits_target)
{
    const auto r = skycued::webapi::makeApiRequest(
        "PUT", "/api/filters?a=1&b=2#frag", {{"Content-Type", "application/json"}, {"X-A", "1"}, {"x-a", "2"}}, "{}");
    BOOST_TEST(r.method == "PUT");
    BOOST_TEST(r.path == "/api/filters");
    BOOST_TEST(r.query == "a=1&b=2");
    BOOST_TEST(r.headers.at("content-type") == "application/json");
    BOOST_TEST(r.headers.at("x-a") == "2");
    BOOST_TEST(r.body == "{}");
    BOOST_TEST(skycued::webapi::makeApiRequest("GET", "/api/state#x", {}, "").query == "");
    // A '?' after the fragment is part of the fragment.
    const auto f = skycued::webapi::makeApiRequest("GET", "/api/state#x?y=1", {}, "");
    BOOST_TEST(f.path == "/api/state");
    BOOST_TEST(f.query == "");
}

BOOST_AUTO_TEST_CASE(snapshot_geom_and_fit_stats_for_a_reported_track)
{
    FakeSource* src = nullptr;
    EngineFx fx(baseConfig(), fakeSource(src));
    TrackObservation o = nearbyObservation(0x402001, 0);
    o.alt_geom_ft = 5400;  // reported with its baro
    fx.t.call([&] {
        src->connection(true);
        src->emit(o);
        for (uint32_t a = 1; a < 3; ++a) {  // the median fallback needs 3 aircraft
            TrackObservation p = nearbyObservation(0x402001 + a, 0);
            p.alt_geom_ft = 5400;
            p.received_time = o.received_time + std::chrono::seconds(6 * a);  // past the fit's 5 s cache
            p.position_time = p.received_time;
            src->emit(p);
        }
    });
    const Snapshot s = fx.t.call([&] { return fx.engine->snapshot(); });
    const TrackSnapshot* t = find(s, icao(0x402001));
    BOOST_REQUIRE(t != nullptr);
    BOOST_TEST((t->geom.source == skycued::GeomSource::reported));
    BOOST_TEST((t->track.alt_geom_ft == std::optional<int32_t>(5400)));
    BOOST_TEST(!t->track.alt_geom_uncertainty_ft.has_value());
    BOOST_TEST(s.stats.altitude_fit.valid);  // three aircraft: the median fallback, not a line
    BOOST_TEST(!s.stats.altitude_fit.linear);
    BOOST_TEST(s.stats.altitude_fit.aircraft == 3u);
    BOOST_TEST(s.stats.altitude_fit.b == 0.0);
}

BOOST_AUTO_TEST_CASE(api_stats_altitude_fit_reaches_the_endpoint)
{
    {
        ApiFx quiet;  // No geometric observation yet: {valid:false} alone.
        auto none = parsed(quiet.get("/api/stats").body).getObject("altitude_fit");
        BOOST_REQUIRE(none);
        BOOST_TEST(none->getString("valid").value_or("") == "false");
        BOOST_TEST(!none->has("samples"));
    }
    ApiFx a;
    TrackObservation o = nearbyObservation(0xABC002, 0);
    o.alt_geom_ft = (o.alt_baro_ft ? *o.alt_baro_ft : 0) + 200;
    // The fit is cached for 5 s of the data clock, and start-up may have asked once already.
    o.received_time += std::chrono::seconds(6);
    o.position_time = o.received_time;
    a.fx->t.call([&] {
        a.src->connection(true);
        a.src->emit(o);
        for (uint32_t k = 1; k < 3; ++k) {  // the median fallback needs 3 aircraft
            TrackObservation p = nearbyObservation(0xABC002 + k, 0);
            p.alt_geom_ft = (p.alt_baro_ft ? *p.alt_baro_ft : 0) + 200;
            p.received_time = o.received_time + std::chrono::seconds(6 * k);  // past the fit's 5 s cache
            p.position_time = p.received_time;
            a.src->emit(p);
        }
    });
    auto fit = parsed(a.get("/api/stats").body).getObject("altitude_fit");
    BOOST_REQUIRE(fit);
    BOOST_TEST(fit->getString("valid").value_or("") == "true");
    BOOST_TEST(fit->has("linear"));
    BOOST_TEST(fit->getDouble("aircraft").value_or(0) == 3.0);
    BOOST_TEST(fit->getDouble("a").value_or(0) == 200.0);
}

// UAT cadence through a real Engine: the decoder's carried altitude times (alt_baro_time,
// alt_geom_time) reach the track, so the 2 s (a baro more than 2 s newer than the geometric
// altitude) and 60 s (a reported value's age) rules of the geometric resolution hold end to end.
BOOST_AUTO_TEST_CASE(uat_cadence_geometric_altitude_rules_hold_end_to_end)
{
    using skycued::testing::encodeUat;
    using skycued::testing::UatSpec;
    using Tp = std::chrono::system_clock::time_point;
    Config cfg = baseConfig();
    cfg.track_timeout_s = 3600;
    FakeSource* src = nullptr;
    EngineFx fx(cfg, fakeSource(src));
    const Tp base = std::chrono::system_clock::now() - std::chrono::minutes(10);

    std::vector<TrackObservation> seen;  // what the decoder produced (io thread only)
    skycued::decode::UatDecoder dec(skycued::decode::DecoderConfig{}, "uat-cadence");
    dec.onObservation([&](const TrackObservation& o) {
        seen.push_back(o);
        src->emit(o);
    });
    auto frame = [&](uint32_t address, unsigned type, unsigned qualifier, int baro, std::optional<int> geom, double at_s) {
        UatSpec u;
        u.type = type;
        u.qualifier = qualifier;
        u.address = address;
        u.lat = 38.95;
        u.lon = -77.0;
        u.alt_ft = baro;
        u.nic = 8;
        u.ns_kt = 100;
        u.ew_kt = 100;
        u.call = "UATCAD";
        u.aux_alt_ft = geom;
        skycued::decode::uat::UatFrame f;
        f.bytes = encodeUat(u);
        dec.feed(f, base + std::chrono::milliseconds(static_cast<long>(at_s * 1000)));
    };
    auto track = [&](TrackId id) {
        const Snapshot snap = fx.t.call([&] { return fx.engine->snapshot(); });
        const TrackSnapshot* t = find(snap, id);
        BOOST_REQUIRE(t != nullptr);
        return *t;
    };
    const TrackId a = icao(0xA10001);
    fx.t.call([&] { src->connection(true); });

    // A long frame: baro and geometric from the same instant.
    fx.t.call([&] { frame(0xA10001, 1, 0, 20000, 20400, 0.0); });
    auto t = track(a);
    BOOST_TEST((t.geom.source == skycued::GeomSource::reported));
    BOOST_TEST((t.track.alt_geom_ft == std::optional<int32_t>(20400)));
    BOOST_TEST((t.geom.geom_minus_baro_ft == std::optional<int32_t>(400)));

    // 1 s later a baro-only frame: the carried geometric altitude is only 1 s older than the baro.
    fx.t.call([&] { frame(0xA10001, 0, 0, 20025, std::nullopt, 1.0); });
    BOOST_REQUIRE(seen.back().alt_geom_ft && seen.back().alt_geom_time);
    BOOST_TEST((*seen.back().alt_geom_time == base));  // the carried time, not the arrival
    t = track(a);
    BOOST_TEST((t.geom.source == skycued::GeomSource::reported));

    // 4 s: the baro is more than 2 s newer than the geometric altitude: held on the own difference.
    fx.t.call([&] { frame(0xA10001, 0, 0, 20100, std::nullopt, 4.0); });
    t = track(a);
    BOOST_TEST((t.geom.source == skycued::GeomSource::estimated));
    BOOST_REQUIRE(t.track.alt_geom_ft.has_value());
    // The own difference: 400 from the long frame, or 375 re-measured by the 1 s frame against its baro.
    BOOST_TEST(*t.track.alt_geom_ft - 20100 >= 375);
    BOOST_TEST(*t.track.alt_geom_ft - 20100 <= 410);
    BOOST_TEST((t.geom.uncertainty_ft == std::optional<int32_t>(75)));

    // The next long frame (2 s cadence of geometric altitudes): reported again, a new difference.
    fx.t.call([&] { frame(0xA10001, 1, 0, 20150, 20575, 5.0); });
    t = track(a);
    BOOST_TEST((t.geom.source == skycued::GeomSource::reported));
    BOOST_TEST((t.track.alt_geom_ft == std::optional<int32_t>(20575)));
    BOOST_TEST((t.geom.geom_minus_baro_ft == std::optional<int32_t>(425)));

    // 65 s after the geometric altitude: the decoder no longer carries it, the track's copy is too old to
    // be reported, and the difference is held (older than 60 s: more uncertain than a fresh one).
    fx.t.call([&] { frame(0xA10001, 0, 0, 20200, std::nullopt, 70.0); });
    BOOST_TEST(!seen.back().alt_geom_ft);
    t = track(a);
    BOOST_TEST((t.geom.source == skycued::GeomSource::estimated));
    BOOST_TEST((t.geom.geom_minus_baro_ft == std::optional<int32_t>(425)));
    BOOST_REQUIRE(t.geom.uncertainty_ft.has_value());
    BOOST_TEST(*t.geom.uncertainty_ft >= 125);

    // A TIS-B trackfile target carries no geometric altitude: it is never reported, and a population
    // fit from three other aircraft estimates it.
    for (int k = 0; k < 3; ++k)
        for (uint32_t i = 0; i < 3; ++i)
            fx.t.call([&] {
                const int baro = 10000 + 5000 * static_cast<int>(i);
                frame(0xA20000 + i, 1, 0, baro, baro + 300, 100.0 + 6.0 * k);
            });
    const TrackId tf{0xA30001, AddressType::tisb_trackfile};
    fx.t.call([&] { frame(0xA30001, 1, 3, 15000, 15300, 130.0); });
    BOOST_REQUIRE(!seen.empty());
    BOOST_TEST(!seen.back().alt_geom_ft);
    BOOST_TEST(!seen.back().geom_minus_baro_ft);
    t = track(tf);
    BOOST_TEST((t.geom.source == skycued::GeomSource::estimated));
    BOOST_TEST(!t.geom.geom_minus_baro_ft.has_value());
    BOOST_REQUIRE(t.track.alt_geom_ft.has_value());
    BOOST_TEST(std::abs(*t.track.alt_geom_ft - 15300) <= 50);
}
