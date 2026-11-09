#define BOOST_TEST_MODULE skycued_change_detector
#include <boost/test/unit_test.hpp>

#include "change_detector.hpp"
#include "geo.hpp"
#include "track_table.hpp"

#include <chrono>
#include <cmath>
#include <ostream>

using skycue::AddressType;
using skycue::Source;
using skycue::TrackId;
using skycued::SendReason;
using skycued::shouldSend;
using skycued::Thresholds;
using skycued::TrackState;
using namespace std::chrono_literals;

namespace skycued {
std::ostream& operator<<(std::ostream& os, SendReason r) { return os << static_cast<int>(r); }
} // namespace skycued

namespace {

const std::chrono::system_clock::time_point kWall{std::chrono::seconds{1700000000}};
const std::chrono::steady_clock::time_point kS0{std::chrono::seconds{1000}};
const Thresholds kDefaults{};

constexpr double kLat = 38.9;
constexpr double kLon = -77.0;

// Degrees of latitude spanning `m` metres on the geo:: sphere.
double latDegForMetres(double m) { return m / (skycued::geo::kEarthRadiusM * 3.14159265358979323846 / 180.0); }

TrackState positioned()
{
    TrackState s;
    s.id = TrackId{0xA12345, AddressType::icao};
    s.source = Source::adsb_1090;
    s.latest.id = s.id;
    s.latest.source = s.source;
    s.latest.received_time = kWall;
    s.latest.position_time = kWall;
    s.latest.lat = kLat;
    s.latest.lon = kLon;
    s.latest.alt_baro_ft = 10000;
    s.latest.alt_geom_ft = 10200;
    s.latest.gs_kt = 250.0f;
    s.latest.track_deg = 90.0f;
    s.latest.callsign = "AAL1";
    s.latest.squawk = "1200";
    s.latest.emergency = "none";
    s.latest.on_ground = false;
    s.last_seen = kWall;
    s.last_position_time = kWall;
    return s;
}

// A state whose current values were just sent at kS0.
TrackState sent()
{
    TrackState s = positioned();
    skycued::markSent(s, skycued::toTrack(s, true, false), kS0);
    return s;
}

} // namespace

BOOST_AUTO_TEST_CASE(first_send)
{
    const TrackState s = positioned();
    BOOST_TEST(shouldSend(s, kS0, kDefaults) == SendReason::first);
    const TrackState t = sent();
    BOOST_TEST(t.last_sent.has_value());
    BOOST_TEST((t.last_sent_at == kS0));
    BOOST_TEST(shouldSend(t, kS0 + 2s, kDefaults) == SendReason::none);
}

BOOST_AUTO_TEST_CASE(no_position_never_sends)
{
    TrackState s = positioned();
    s.latest.lat.reset();
    s.latest.lon.reset();
    s.latest.position_time.reset();
    BOOST_TEST(shouldSend(s, kS0, kDefaults) == SendReason::none);
    s.latest.callsign = "NEW1";
    BOOST_TEST(shouldSend(s, kS0 + 100s, kDefaults) == SendReason::none);
}

BOOST_AUTO_TEST_CASE(min_interval_blocks_small_change)
{
    TrackState s = sent();
    *s.latest.lat += latDegForMetres(100.0);
    *s.latest.alt_baro_ft += 500;
    BOOST_TEST(shouldSend(s, kS0 + 500ms, kDefaults) == SendReason::none);
    BOOST_TEST(shouldSend(s, kS0 + 999ms, kDefaults) == SendReason::none);
    BOOST_TEST(shouldSend(s, kS0 + 1s, kDefaults) == SendReason::moved);
}

BOOST_AUTO_TEST_CASE(identity_bypasses_min_interval)
{
    {
        TrackState s = sent();
        s.latest.callsign = "AAL2";
        BOOST_TEST(shouldSend(s, kS0 + 100ms, kDefaults) == SendReason::identity);
    }
    {
        TrackState s = sent();
        s.latest.squawk = "7700";
        BOOST_TEST(shouldSend(s, kS0 + 100ms, kDefaults) == SendReason::identity);
    }
    {
        TrackState s = sent();
        s.latest.emergency = "general";
        BOOST_TEST(shouldSend(s, kS0 + 100ms, kDefaults) == SendReason::identity);
    }
    {
        TrackState s = sent();
        s.latest.on_ground = true;
        BOOST_TEST(shouldSend(s, kS0 + 100ms, kDefaults) == SendReason::identity);
    }
    {
        // Identity also wins over a movement once the interval has passed.
        TrackState s = sent();
        *s.latest.lat += latDegForMetres(100.0);
        s.latest.callsign = "AAL2";
        BOOST_TEST(shouldSend(s, kS0 + 5s, kDefaults) == SendReason::identity);
    }
}

BOOST_AUTO_TEST_CASE(ra_change_is_immediate)
{
    skycued::AcasRaState ra;
    ra.active = true;
    ra.ara = 0x2000;
    {
        TrackState s = sent();
        s.latest.acas_ra = ra;
        BOOST_TEST(shouldSend(s, kS0 + 100ms, kDefaults) == SendReason::identity);
    }
    {
        TrackState s = positioned();
        s.latest.acas_ra = ra;
        skycued::markSent(s, skycued::toTrack(s, true, false), kS0);
        BOOST_TEST(shouldSend(s, kS0 + 100ms, kDefaults) == SendReason::none);
        s.latest.acas_ra->ara = 0x4000; // a different advisory
        BOOST_TEST(shouldSend(s, kS0 + 100ms, kDefaults) == SendReason::identity);
        s = positioned();
        s.latest.acas_ra = ra;
        skycued::markSent(s, skycued::toTrack(s, true, false), kS0);
        s.latest.acas_ra->active = false; // RAT: the advisory ended
        BOOST_TEST(shouldSend(s, kS0 + 100ms, kDefaults) == SendReason::identity);
    }
}

BOOST_AUTO_TEST_CASE(emergency_compared_through_track)
{
    TrackState s = sent();
    BOOST_REQUIRE(s.last_sent->emergency.has_value());
    BOOST_TEST(int(*s.last_sent->emergency) == 0);
    s.latest.emergency = "medical";
    BOOST_TEST(shouldSend(s, kS0 + 100ms, kDefaults) == SendReason::identity);
    skycued::markSent(s, skycued::toTrack(s, false, false), kS0 + 100ms);
    BOOST_TEST(int(*s.last_sent->emergency) == 2);
    BOOST_TEST(shouldSend(s, kS0 + 200ms, kDefaults) == SendReason::none);
}

BOOST_AUTO_TEST_CASE(on_ground_unset_on_both_sides_is_no_change)
{
    // An input that never said whether the target is on the ground: it was
    // sent as airborne (false), and unset still reads as airborne.
    TrackState s = sent();
    BOOST_TEST(s.last_sent->on_ground == false);
    s.latest.on_ground.reset();
    BOOST_TEST(shouldSend(s, kS0 + 100ms, kDefaults) == SendReason::none);
}

BOOST_AUTO_TEST_CASE(moved_10m_sends)
{
    TrackState s = sent();
    *s.latest.lat = kLat + latDegForMetres(11.0);
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::moved);
    *s.latest.lat = kLat + latDegForMetres(5.0);
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::none);
}

BOOST_AUTO_TEST_CASE(altitude_50ft)
{
    TrackState s = sent();
    s.latest.alt_baro_ft = 10049;
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::none);
    s.latest.alt_baro_ft = 10050;
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::altitude);
    s.latest.alt_baro_ft = 9950;
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::altitude);

    // Baro present: a geometric change alone does not count.
    s.latest.alt_baro_ft = 10000;
    s.latest.alt_geom_ft = 11000;
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::none);

    // Baro absent: geometric altitude is used.
    TrackState g = positioned();
    g.latest.alt_baro_ft.reset();
    skycued::markSent(g, skycued::toTrack(g, true, true), kS0);
    g.latest.alt_geom_ft = 10249;
    BOOST_TEST(shouldSend(g, kS0 + 2s, kDefaults) == SendReason::none);
    g.latest.alt_geom_ft = 10250;
    BOOST_TEST(shouldSend(g, kS0 + 2s, kDefaults) == SendReason::altitude);
}

BOOST_AUTO_TEST_CASE(speed_5kt)
{
    TrackState s = sent();
    s.latest.gs_kt = 254.5f;
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::none);
    s.latest.gs_kt = 245.0f;
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::speed);
}

BOOST_AUTO_TEST_CASE(track_wrap_359_to_1_is_2deg)
{
    TrackState s = positioned();
    s.latest.track_deg = 359.0f;
    skycued::markSent(s, skycued::toTrack(s, true, false), kS0);
    s.latest.track_deg = 1.0f;
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::track);
    s.latest.track_deg = 0.0f; // 1 degree
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::none);
    s.latest.track_deg = 357.0f;
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::track);

    TrackState t = positioned();
    t.latest.track_deg = 1.0f;
    skycued::markSent(t, skycued::toTrack(t, true, false), kS0);
    t.latest.track_deg = 359.5f; // 1.5 degrees
    BOOST_TEST(shouldSend(t, kS0 + 2s, kDefaults) == SendReason::none);
    t.latest.track_deg = 359.0f;
    BOOST_TEST(shouldSend(t, kS0 + 2s, kDefaults) == SendReason::track);
}

BOOST_AUTO_TEST_CASE(reason_order_moved_altitude_speed_track)
{
    TrackState s = sent();
    *s.latest.lat += latDegForMetres(100.0);
    s.latest.alt_baro_ft = 11000;
    s.latest.gs_kt = 300.0f;
    s.latest.track_deg = 180.0f;
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::moved);
    *s.latest.lat = kLat;
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::altitude);
    s.latest.alt_baro_ft = 10000;
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::speed);
    s.latest.gs_kt = 250.0f;
    BOOST_TEST(shouldSend(s, kS0 + 2s, kDefaults) == SendReason::track);
}

BOOST_AUTO_TEST_CASE(max_interval_sends_unchanged)
{
    const TrackState s = sent();
    BOOST_TEST(shouldSend(s, kS0 + 9900ms, kDefaults) == SendReason::none);
    BOOST_TEST(shouldSend(s, kS0 + 10s, kDefaults) == SendReason::max_interval);
    Thresholds t;
    t.max_interval_s = 3.0;
    BOOST_TEST(shouldSend(s, kS0 + 3s, t) == SendReason::max_interval);
}

BOOST_AUTO_TEST_CASE(to_track_copies_every_field)
{
    TrackState s = positioned();
    s.latest.on_ground = true;
    s.latest.registration = "N12345";
    s.latest.nic = 8;
    s.latest.nac_p = 9;
    s.latest.baro_rate_fpm = -640.0f;
    s.latest.geom_rate_fpm = -600.0f;
    s.last_position_time = kWall - 2s;
    s.latest.heading_mag_deg = 91.0f;
    s.latest.heading_true_deg = 92.0f;
    s.latest.ias_kt = 250;
    s.latest.tas_kt = 400.0f;
    s.latest.mach = 0.7f;
    s.latest.roll_deg = -5.0f;
    s.latest.track_rate_dps = 1.5f;
    s.latest.sel_alt_ft = 12000;
    s.latest.sel_alt_fms_ft = 12500;
    s.latest.qnh_mb = 1009.0f;
    s.latest.sel_heading_deg = 95.0f;
    s.latest.nav_modes = skycued::kNavAutopilot | skycued::kNavTcas;
    s.latest.category = "A3";
    s.latest.emergency = "downed_aircraft";
    skycued::AcasRaState ra;
    ra.active = true;
    ra.ara = 0x1234;
    ra.rac = 5;
    ra.rat = true;
    ra.mte = true;
    s.latest.acas_ra = ra;

    const skycue::Track t = skycued::toTrack(s, true, true);
    BOOST_TEST(*t.heading_mag_deg == 91.0f);
    BOOST_TEST(*t.heading_true_deg == 92.0f);
    BOOST_TEST(*t.ias_kt == 250);
    BOOST_TEST(*t.tas_kt == 400);
    BOOST_TEST(*t.mach == 0.7f);
    BOOST_TEST(*t.roll_deg == -5.0f);
    BOOST_TEST(*t.track_rate_dps == 1.5f);
    BOOST_TEST(*t.sel_alt_ft == 12000);
    BOOST_TEST(*t.sel_alt_fms_ft == 12500);
    BOOST_TEST(*t.qnh_mb == 1009.0f);
    BOOST_TEST(*t.sel_heading_deg == 95.0f);
    BOOST_TEST(int(*t.nav_modes) == 0x21);
    BOOST_TEST(*t.category == "A3");
    BOOST_TEST(int(*t.emergency) == 6);
    BOOST_REQUIRE(t.acas_ra.has_value());
    BOOST_TEST(t.acas_ra->ara == 0x1234);
    BOOST_TEST(int(t.acas_ra->flags) == (5 | 0x10 | 0x20 | 0x40));
    BOOST_TEST(t.id.address == 0xA12345u);
    BOOST_TEST((t.id.type == AddressType::icao));
    BOOST_TEST((t.source == Source::adsb_1090));
    BOOST_TEST((t.position_time == kWall - 2s));
    BOOST_TEST(t.lat_deg == kLat);
    BOOST_TEST(t.lon_deg == kLon);
    BOOST_TEST(t.on_ground);
    BOOST_TEST(t.keyframe);
    BOOST_TEST(t.alt_is_geom);
    BOOST_TEST(*t.alt_baro_ft == 10000);
    BOOST_TEST(*t.alt_geom_ft == 10200);
    BOOST_TEST(*t.ground_speed_kt == 250.0f);
    BOOST_TEST(*t.track_deg == 90.0f);
    BOOST_TEST(*t.vertical_rate_fpm == -640.0f);
    BOOST_TEST(*t.callsign == "AAL1");
    BOOST_TEST(*t.squawk == "1200");
    BOOST_TEST(*t.registration == "N12345");
    BOOST_TEST(*t.nic == 8);
    BOOST_TEST(*t.nac_p == 9);

    s.latest.baro_rate_fpm.reset();
    const skycue::Track g = skycued::toTrack(s, false, false);
    BOOST_TEST(*g.vertical_rate_fpm == -600.0f);
    BOOST_TEST(!g.keyframe);
    BOOST_TEST(!g.alt_is_geom);

    s.latest.geom_rate_fpm.reset();
    BOOST_TEST(!skycued::toTrack(s, false, false).vertical_rate_fpm.has_value());
}
