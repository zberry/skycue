#define BOOST_TEST_MODULE skycued_engine_api
#include <boost/test/unit_test.hpp>

#include "engine_events.hpp"
#include "engine_test_support.hpp"
#include "filters.hpp"

#include <cmath>
#include <map>

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
        e.onEvent([this](const EngineEvent& ev) { events.push_back(ev); });
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
    const std::vector<EventKind> head{EventKind::update, EventKind::filters_changed, EventKind::drop,
                                      EventKind::lists_changed};
    BOOST_REQUIRE(kinds.size() >= head.size());
    BOOST_TEST((std::vector<EventKind>(kinds.begin(), kinds.begin() + 4) == head));
    fx.t.call([&] {
        const EngineEvent& up = sink.events[0];
        BOOST_REQUIRE(up.track);
        BOOST_TEST((up.track->track.id == id));
        BOOST_TEST(up.track->forwarded);
        BOOST_TEST((up.track->decision == Decision::send));
        const EngineEvent& dr = sink.events[2];
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
