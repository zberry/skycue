#define BOOST_TEST_MODULE skycued_track_table
#include <boost/test/unit_test.hpp>

#include "observation.hpp"
#include "track_table.hpp"

#include <chrono>
#include <string>

using skycue::AddressType;
using skycue::Source;
using skycue::TrackId;
using skycued::TrackObservation;
using skycued::TrackTable;
using namespace std::chrono_literals;

namespace {

const std::chrono::system_clock::time_point kT0{std::chrono::seconds{1700000000}};

TrackObservation obs(uint32_t address, std::chrono::system_clock::time_point t, bool with_position)
{
    TrackObservation o;
    o.id = TrackId{address, AddressType::icao};
    o.source = Source::adsb_1090;
    o.received_time = t;
    if (with_position) {
        o.lat = 38.9;
        o.lon = -77.0;
        o.position_time = t - 1s;
    }
    o.raw_line = "line@" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(t - kT0).count());
    return o;
}

} // namespace

BOOST_AUTO_TEST_CASE(merge_keeps_last_known_optional)
{
    TrackTable table;
    auto a = obs(0xABC123, kT0, true);
    a.callsign = "UAL123";
    a.squawk = "1200";
    a.alt_baro_ft = 35000;
    a.gs_kt = 450.0f;
    a.nic = 8;
    a.rssi = -20.5;
    table.apply(a);

    auto b = obs(0xABC123, kT0 + 5s, false); // no position, no callsign
    b.alt_baro_ft = 35100;
    b.squawk = "7000";
    auto& s = table.apply(b);

    BOOST_TEST(table.size() == 1u);
    BOOST_REQUIRE(s.latest.callsign.has_value());
    BOOST_TEST(*s.latest.callsign == "UAL123");
    BOOST_TEST(*s.latest.squawk == "7000");
    BOOST_TEST(*s.latest.alt_baro_ft == 35100);
    BOOST_TEST(*s.latest.gs_kt == 450.0f);
    BOOST_TEST(*s.latest.nic == 8);
    BOOST_TEST(*s.latest.rssi == -20.5);
    // Position (and its time) survives an observation without one.
    BOOST_REQUIRE(s.latest.lat.has_value());
    BOOST_TEST(*s.latest.lat == 38.9);
    BOOST_TEST(*s.latest.lon == -77.0);
    BOOST_TEST((*s.latest.position_time == kT0 - 1s));
    BOOST_TEST((s.last_position_time == kT0 - 1s));
    BOOST_TEST((s.last_seen == kT0 + 5s));
    BOOST_TEST((s.latest.received_time == kT0 + 5s));
    BOOST_TEST(s.latest.raw_line == b.raw_line);

    // A later position updates last_position_time.
    auto c = obs(0xABC123, kT0 + 9s, true);
    c.lat = 39.0;
    auto& s2 = table.apply(c);
    BOOST_TEST(&s2 == &s);
    BOOST_TEST(*s2.latest.lat == 39.0);
    BOOST_TEST((s2.last_position_time == kT0 + 8s));
}

BOOST_AUTO_TEST_CASE(on_ground_kept_when_alt_baro_absent)
{
    TrackTable table;
    auto ground = obs(0xABC124, kT0, true);
    ground.on_ground = true; // alt_baro "ground"
    BOOST_TEST((table.apply(ground).latest.on_ground == true));

    // No alt_baro at all: says nothing about on_ground, which stays true.
    auto silent = obs(0xABC124, kT0 + 1s, true);
    BOOST_TEST((table.apply(silent).latest.on_ground == true));

    // A numeric alt_baro means airborne (the readsb parser asserts false).
    auto airborne = obs(0xABC124, kT0 + 2s, true);
    airborne.alt_baro_ft = 1200;
    airborne.on_ground = false;
    BOOST_TEST(!table.apply(airborne).latest.on_ground.value_or(true));

    // And again absent alt_baro keeps it airborne; "ground" flips it back.
    BOOST_TEST(!table.apply(obs(0xABC124, kT0 + 3s, true)).latest.on_ground.value_or(true));
    auto back = obs(0xABC124, kT0 + 4s, true);
    back.on_ground = true;
    BOOST_TEST((table.apply(back).latest.on_ground == true));
}

BOOST_AUTO_TEST_CASE(on_ground_absent_keeps_last_value)
{
    TrackTable table;
    auto ground = obs(0xABC125, kT0, true);
    ground.on_ground = true;
    table.apply(ground);

    // An input that does not carry the flag (unset) but has an altitude
    // leaves the last known value alone.
    auto silent = obs(0xABC125, kT0 + 1s, true);
    silent.alt_baro_ft = 1200;
    const auto& s = table.apply(silent);
    BOOST_TEST((s.latest.on_ground == true));
    BOOST_TEST(*s.latest.alt_baro_ft == 1200);
}

BOOST_AUTO_TEST_CASE(on_ground_false_asserted_clears)
{
    TrackTable table;
    auto ground = obs(0xABC126, kT0, true);
    ground.on_ground = true;
    table.apply(ground);

    auto airborne = obs(0xABC126, kT0 + 1s, true);
    airborne.on_ground = false;
    BOOST_TEST((table.apply(airborne).latest.on_ground == false));
}

BOOST_AUTO_TEST_CASE(last_input_follows_newest)
{
    TrackTable table;
    auto a = obs(0xABC127, kT0, true);
    a.input_id = "readsb";
    BOOST_TEST(table.apply(a).last_input == "readsb");

    auto b = obs(0xABC127, kT0 + 1s, true);
    b.input_id = "ps-north";
    BOOST_TEST(table.apply(b).last_input == "ps-north");

    auto c = obs(0xABC127, kT0 + 2s, false);
    c.input_id = "readsb";
    BOOST_TEST(table.apply(c).last_input == "readsb");
}

BOOST_AUTO_TEST_CASE(new_track_fields)
{
    TrackTable table;
    auto& s = table.apply(obs(0x000001, kT0, true));
    BOOST_TEST(s.id.address == 0x000001u);
    BOOST_TEST((s.source == Source::adsb_1090));
    BOOST_TEST(!s.last_sent.has_value());
    BOOST_TEST(!s.currently_forwarded);
    BOOST_TEST(table.find(TrackId{0x000001, AddressType::icao}) == &s);
    BOOST_TEST(table.find(TrackId{0x000001, AddressType::tisb_trackfile}) == nullptr);
    int n = 0;
    table.forEach([&](skycued::TrackState&) { ++n; });
    BOOST_TEST(n == 1);
}

BOOST_AUTO_TEST_CASE(expire_by_position_age)
{
    TrackTable table;
    table.apply(obs(0x1, kT0, true));          // position time kT0-1s
    table.apply(obs(0x1, kT0 + 50s, false));   // still seen, but no new position
    table.apply(obs(0x2, kT0 + 40s, true));    // position time kT0+39s

    // At kT0+60s with a 60 s timeout, track 1's position (kT0-1s) is 61 s old.
    const auto gone = table.expire(kT0 + 60s, 60s);
    BOOST_REQUIRE(gone.size() == 1u);
    BOOST_TEST(gone[0].address == 0x1u);
    BOOST_TEST(table.size() == 1u);
    BOOST_TEST(table.find(TrackId{0x1, AddressType::icao}) == nullptr);
    BOOST_TEST(table.find(TrackId{0x2, AddressType::icao}) != nullptr);

    // Exactly at the timeout is not yet older than it.
    BOOST_TEST(table.expire(kT0 + 99s, 60s).empty());
    BOOST_TEST(table.expire(kT0 + 100s, 60s).size() == 1u);
    BOOST_TEST(table.size() == 0u);
}

BOOST_AUTO_TEST_CASE(expire_never_positioned_by_seen)
{
    TrackTable table;
    table.apply(obs(0x3, kT0, false));
    table.apply(obs(0x3, kT0 + 30s, false));
    BOOST_TEST(table.expire(kT0 + 60s, 60s).empty()); // last_seen is 30 s old
    const auto gone = table.expire(kT0 + 91s, 60s);
    BOOST_REQUIRE(gone.size() == 1u);
    BOOST_TEST(gone[0].address == 0x3u);
    BOOST_TEST(table.size() == 0u);
}

BOOST_AUTO_TEST_CASE(rebroadcast_position_suppressed_while_direct_fresh)
{
    TrackTable table;
    auto d = obs(0xABC123, kT0, true);
    d.lat = 38.9;
    d.lon = -77.0;
    table.apply(d);
    BOOST_TEST(!table.lastApplySuppressedPosition());

    auto r = obs(0xABC123, kT0 + 100ms, true);
    r.rebroadcast = true;
    r.source = Source::tisb;
    r.lat = 39.5;
    r.lon = -76.0;
    r.callsign = "TISB1";
    const auto& s = table.apply(r);
    BOOST_TEST(table.lastApplySuppressedPosition());
    BOOST_TEST(*s.latest.lat == 38.9);
    BOOST_TEST(*s.latest.lon == -77.0);
    BOOST_TEST(*s.latest.callsign == "TISB1");  // other fields still merge
    BOOST_TEST(static_cast<int>(s.source) == static_cast<int>(Source::adsb_1090));

    auto late = obs(0xABC123, kT0 + 10500ms, true);
    late.rebroadcast = true;
    late.source = Source::tisb;
    late.lat = 39.5;
    late.lon = -76.0;
    const auto& s2 = table.apply(late);
    BOOST_TEST(!table.lastApplySuppressedPosition());
    BOOST_TEST(*s2.latest.lat == 39.5);
}

BOOST_AUTO_TEST_CASE(direct_position_always_applies)
{
    TrackTable table;
    auto d = obs(0xABC123, kT0, true);
    table.apply(d);
    auto r = obs(0xABC123, kT0 + 1s, true);
    r.rebroadcast = true;
    table.apply(r);
    auto d2 = obs(0xABC123, kT0 + 2s, true);
    d2.lat = 40.0;
    d2.lon = -78.0;
    const auto& s = table.apply(d2);
    BOOST_TEST(!table.lastApplySuppressedPosition());
    BOOST_TEST(*s.latest.lat == 40.0);
}

BOOST_AUTO_TEST_CASE(rebroadcast_alone_is_not_suppressed)
{
    TrackTable table;
    auto r = obs(0xABC124, kT0, true);
    r.rebroadcast = true;
    r.source = Source::tisb;
    const auto& s = table.apply(r);
    BOOST_TEST(!table.lastApplySuppressedPosition());
    BOOST_TEST(s.latest.lat.has_value());
}

BOOST_AUTO_TEST_CASE(altitude_times_follow_the_carried_value)
{
    TrackTable table;
    TrackObservation a = obs(0xA1, kT0, false);
    a.alt_baro_ft = 30000;
    a.alt_baro_time = kT0 - 4s;
    a.alt_geom_ft = 30500;
    table.apply(a);
    auto* s = table.find(a.id);
    BOOST_REQUIRE(s);
    BOOST_CHECK(s->alt_baro_at == kT0 - 4s);
    BOOST_CHECK(s->alt_geom_at == kT0);

    // An observation without a value leaves the time alone.
    TrackObservation b = obs(0xA1, kT0 + 10s, false);
    b.alt_baro_ft = 30100;
    table.apply(b);
    s = table.find(a.id);
    BOOST_CHECK(s->alt_baro_at == kT0 + 10s);
    BOOST_CHECK(s->alt_geom_at == kT0);
}
