#define BOOST_TEST_MODULE skycue_ecef
#include <boost/test/unit_test.hpp>

#include <skycue/ecef.hpp>

#include <atomic>
#include <cmath>
#include <thread>

using namespace skycue::geo;

namespace {
constexpr double kTol = 1e-3; // metres
}

BOOST_AUTO_TEST_CASE(equator_prime_meridian)
{
    const Vec3 p = geodeticToEcef(0, 0, 0);
    BOOST_CHECK_SMALL(p.x - 6378137.0, kTol);
    BOOST_CHECK_SMALL(p.y, kTol);
    BOOST_CHECK_SMALL(p.z, kTol);
}

BOOST_AUTO_TEST_CASE(equator_90e)
{
    const Vec3 p = geodeticToEcef(0, 90, 0);
    BOOST_CHECK_SMALL(p.x, kTol);
    BOOST_CHECK_SMALL(p.y - 6378137.0, kTol);
    BOOST_CHECK_SMALL(p.z, kTol);
}

BOOST_AUTO_TEST_CASE(north_pole)
{
    // Semi-minor axis b = a(1 - f) = 6356752.3142 m.
    const Vec3 p = geodeticToEcef(90, 0, 0);
    BOOST_CHECK_SMALL(p.x, kTol);
    BOOST_CHECK_SMALL(p.y, kTol);
    BOOST_CHECK_SMALL(p.z - 6356752.3142, kTol);
}

BOOST_AUTO_TEST_CASE(reference_point)
{
    // Reference point 48.8583 N, 2.2945 E, ellipsoidal height 35 m, with the
    // WGS-84 constants a = 6378137 m, 1/f = 298.257223563 (NIMA TR8350.2,
    // 3rd ed., 2000, section 3.2 / appendix, geodetic to ECEF equations).
    // The expected values were evaluated from those published equations at 40
    // digits of precision (Python mpmath), independently of the C++ code:
    //   X = 4200967.195383 m, Y = 168324.359368 m, Z = 4780232.083709 m
    const Vec3 p = geodeticToEcef(48.8583, 2.2945, 35.0);
    BOOST_CHECK_SMALL(p.x - 4200967.195383, kTol);
    BOOST_CHECK_SMALL(p.y - 168324.359368, kTol);
    BOOST_CHECK_SMALL(p.z - 4780232.083709, kTol);
}

BOOST_AUTO_TEST_CASE(southern_western_hemisphere_point)
{
    // Santiago, 33.8568 S, 70.6693 W, 500 m; evaluated from the same published equations.
    const Vec3 p = geodeticToEcef(-33.8568, -70.6693, 500.0);
    BOOST_CHECK_SMALL(p.x - 1755246.8025, kTol);
    BOOST_CHECK_SMALL(p.y + 5003601.8195, kTol);
    BOOST_CHECK_SMALL(p.z + 3533545.6871, kTol);
}

BOOST_AUTO_TEST_CASE(ned_axes_at_origin)
{
    const Vec3 n = nedToEcef(0, 0, 1, 0, 0);
    BOOST_CHECK_SMALL(n.x, 1e-12);
    BOOST_CHECK_SMALL(n.y, 1e-12);
    BOOST_CHECK_CLOSE(n.z, 1.0, 1e-9);
    const Vec3 e = nedToEcef(0, 0, 0, 1, 0);
    BOOST_CHECK_SMALL(e.x, 1e-12);
    BOOST_CHECK_CLOSE(e.y, 1.0, 1e-9);
    BOOST_CHECK_SMALL(e.z, 1e-12);
    const Vec3 d = nedToEcef(0, 0, 0, 0, 1);
    BOOST_CHECK_CLOSE(d.x, -1.0, 1e-9);
    BOOST_CHECK_SMALL(d.y, 1e-12);
    BOOST_CHECK_SMALL(d.z, 1e-12);
}

BOOST_AUTO_TEST_CASE(look_north_and_up)
{
    const double lat = 45, lon = 10;
    const Vec3 sensor = geodeticToEcef(lat, lon, 0);
    // 1000 m north and 1000 m up, built in the sensor's local frame.
    const Vec3 north = nedToEcef(lat, lon, 1000, 0, 0);
    const Vec3 up = nedToEcef(lat, lon, 0, 0, -1000);
    const Vec3 target{sensor.x + north.x + up.x, sensor.y + north.y + up.y, sensor.z + north.z + up.z};
    const LookAngles l = lookFrom(sensor, target);
    BOOST_CHECK_SMALL(l.azimuth_deg > 180 ? l.azimuth_deg - 360 : l.azimuth_deg, 0.01);
    BOOST_CHECK_SMALL(l.elevation_deg - 45.0, 0.01);
    BOOST_CHECK_SMALL(l.range_m - 1414.2, 1.0);
}

BOOST_AUTO_TEST_CASE(poles_and_dateline_finite)
{
    const double lats[] = {90, -90, 0, 89.9999999};
    const double lons[] = {180, -180, 0, 179.9999999};
    for (double la : lats)
        for (double lo : lons) {
            const Vec3 p = geodeticToEcef(la, lo, 10000);
            BOOST_CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
            const Vec3 v = nedToEcef(la, lo, 100, 100, 5);
            BOOST_CHECK(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z));
            const LookAngles l = lookFrom(p, geodeticToEcef(la, lo, 20000));
            BOOST_CHECK(std::isfinite(l.azimuth_deg) && std::isfinite(l.elevation_deg));
            BOOST_CHECK_GE(l.azimuth_deg, 0.0);
            BOOST_CHECK_LT(l.azimuth_deg, 360.0);
            BOOST_CHECK_CLOSE(l.elevation_deg, 90.0, 1e-6);
            BOOST_CHECK_SMALL(l.range_m - 10000.0, 1e-3);
        }
}

BOOST_AUTO_TEST_CASE(propagate_adds_v_dt)
{
    EcefState s;
    s.time = std::chrono::system_clock::time_point{} + std::chrono::seconds(1000);
    s.position = {1000, 2000, 3000};
    s.velocity = Vec3{10, -20, 5};
    const Vec3 p = propagate(s, s.time + std::chrono::milliseconds(2500));
    BOOST_CHECK_CLOSE(p.x, 1025.0, 1e-9);
    BOOST_CHECK_CLOSE(p.y, 1950.0, 1e-9);
    BOOST_CHECK_CLOSE(p.z, 3012.5, 1e-9);
    // No velocity: the position is returned unmoved.
    s.velocity.reset();
    const Vec3 q = propagate(s, s.time + std::chrono::seconds(60));
    BOOST_CHECK_EQUAL(q.x, 1000.0);
    BOOST_CHECK_EQUAL(q.y, 2000.0);
    BOOST_CHECK_EQUAL(q.z, 3000.0);
}

// ---- ladders, toEcef and EcefTracker ----

namespace {
using skycue::Track;
using skycue::TrackId;
using Clock = std::chrono::system_clock;
using SClock = std::chrono::steady_clock;

Clock::time_point at(int ms) { return Clock::time_point{} + std::chrono::milliseconds(1000000 + ms); }

Track mk(uint32_t addr = 1, int t_ms = 0, double lat = 0, double lon = 0)
{
    Track t;
    t.id = TrackId{addr, skycue::AddressType::icao};
    t.position_time = at(t_ms);
    t.lat_deg = lat;
    t.lon_deg = lon;
    t.alt_geom_ft = 10000;
    return t;
}
TrackId idOf(uint32_t a) { return TrackId{a, skycue::AddressType::icao}; }
constexpr double kM100North = 100.0 / 110574.3; // degrees of latitude for 100 m at the equator
} // namespace

BOOST_AUTO_TEST_CASE(altitude_geom_reported)
{
    const auto s = toEcef(mk());
    BOOST_REQUIRE(s);
    BOOST_CHECK(s->altitude_source == AltitudeSource::geom_reported);
    BOOST_CHECK_EQUAL(s->vertical_sigma_m, 15.0);
    BOOST_CHECK_SMALL(s->position.x - (6378137.0 + 3048.0), kTol);
}

BOOST_AUTO_TEST_CASE(altitude_geom_estimated_sigma_from_wire)
{
    Track t = mk();
    t.alt_geom_uncertainty_ft = 100;
    const auto s = toEcef(t);
    BOOST_REQUIRE(s);
    BOOST_CHECK(s->altitude_source == AltitudeSource::geom_estimated);
    BOOST_CHECK_CLOSE(s->vertical_sigma_m, 30.48, 1e-9);
}

BOOST_AUTO_TEST_CASE(altitude_baro_sigma_300)
{
    Track t = mk();
    t.alt_geom_ft.reset();
    t.alt_baro_ft = 5000;
    const auto s = toEcef(t);
    BOOST_REQUIRE(s);
    BOOST_CHECK(s->altitude_source == AltitudeSource::baro);
    BOOST_CHECK_EQUAL(s->vertical_sigma_m, 300.0);
    Options o;
    o.baro_sigma_m = 450;
    BOOST_CHECK_EQUAL(toEcef(t, o)->vertical_sigma_m, 450.0);
}

BOOST_AUTO_TEST_CASE(altitude_caller_default_only_when_set)
{
    Track t = mk();
    t.alt_geom_ft.reset();
    t.on_ground = true;
    Options o;
    o.default_height_m = 120.0;
    const auto s = toEcef(t, o);
    BOOST_REQUIRE(s);
    BOOST_CHECK(s->altitude_source == AltitudeSource::caller_default);
    BOOST_CHECK_EQUAL(s->vertical_sigma_m, 30.0);
    BOOST_CHECK_SMALL(s->position.x - (6378137.0 + 120.0), kTol);
}

BOOST_AUTO_TEST_CASE(no_altitude_rung_gives_nullopt)
{
    Track t = mk();
    t.alt_geom_ft.reset();
    BOOST_CHECK(!toEcef(t));
}

BOOST_AUTO_TEST_CASE(velocity_reported_from_gs_track)
{
    Track t = mk();
    t.ground_speed_kt = 100.0f;
    t.track_deg = 90.0f;
    const auto s = toEcef(t);
    BOOST_REQUIRE(s && s->velocity);
    BOOST_CHECK(s->velocity_source == VelocitySource::reported);
    BOOST_CHECK_SMALL(s->velocity->x, 1e-6);
    BOOST_CHECK_CLOSE(s->velocity->y, 51.4444, 1e-6);
    BOOST_CHECK_SMALL(s->velocity->z, 1e-6);
    BOOST_CHECK(s->vertical_source == VerticalSource::assumed_zero);
}

BOOST_AUTO_TEST_CASE(velocity_none_without_track)
{
    Track t = mk();
    t.ground_speed_kt = 100.0f;
    t.vertical_rate_fpm = 500.0f;
    const auto s = toEcef(t);
    BOOST_REQUIRE(s);
    BOOST_CHECK(!s->velocity);
    BOOST_CHECK(s->velocity_source == VelocitySource::none);
    BOOST_CHECK(s->vertical_source == VerticalSource::assumed_zero); // ladder skipped
}

BOOST_AUTO_TEST_CASE(tracker_derives_from_two_fixes)
{
    Options o;
    o.derive_velocity = true;
    EcefTracker tr(o);
    tr.update(mk(1, 0, 0, 0));
    const auto s = tr.update(mk(1, 1000, kM100North, 0));
    BOOST_REQUIRE(s && s->velocity);
    BOOST_CHECK(s->velocity_source == VelocitySource::derived);
    // At (0,0) north is +z.
    BOOST_CHECK_CLOSE(s->velocity->z, 100.0, 0.5);
    BOOST_CHECK_SMALL(s->velocity->x, 0.05);
    BOOST_CHECK_SMALL(s->velocity->y, 0.05);
}

BOOST_AUTO_TEST_CASE(tracker_needs_option)
{
    EcefTracker tr;
    tr.update(mk(1, 0, 0, 0));
    const auto s = tr.update(mk(1, 1000, kM100North, 0));
    BOOST_REQUIRE(s);
    BOOST_CHECK(s->velocity_source == VelocitySource::none);
}

BOOST_AUTO_TEST_CASE(tracker_min_separation)
{
    Options o;
    o.derive_velocity = true;
    EcefTracker tr(o);
    tr.update(mk(1, 0, 0, 0));
    BOOST_CHECK(tr.update(mk(1, 1000, kM100North * 0.4, 0))->velocity_source == VelocitySource::none); // 40 m
}

BOOST_AUTO_TEST_CASE(tracker_max_fix_age)
{
    Options o;
    o.derive_velocity = true;
    EcefTracker tr(o);
    tr.update(mk(1, 0, 0, 0));
    BOOST_CHECK(tr.update(mk(1, 11000, kM100North, 0))->velocity_source == VelocitySource::none);
}

BOOST_AUTO_TEST_CASE(keyframe_same_time_does_not_derive)
{
    Options o;
    o.derive_velocity = true;
    EcefTracker tr(o);
    tr.update(mk(1, 0, 0, 0));
    Track k = mk(1, 0, kM100North, 0);
    k.keyframe = true;
    BOOST_CHECK(tr.update(k)->velocity_source == VelocitySource::none);
}

BOOST_AUTO_TEST_CASE(vertical_reported_fpm)
{
    Track t = mk();
    t.ground_speed_kt = 100.0f;
    t.track_deg = 0.0f;
    t.vertical_rate_fpm = 1000.0f;
    const auto s = toEcef(t);
    BOOST_REQUIRE(s && s->velocity);
    BOOST_CHECK(s->vertical_source == VerticalSource::reported);
    // At (0,0) up is +x.
    BOOST_CHECK_CLOSE(s->velocity->x, 5.08, 1e-6);
}

BOOST_AUTO_TEST_CASE(vertical_derived_same_rung_only)
{
    Options o;
    o.derive_velocity = true;
    EcefTracker tr(o);
    Track a = mk(1, 0, 0, 0);
    tr.update(a);
    Track b = mk(1, 1000, kM100North, 0);
    b.alt_geom_ft = 10000 + 328; // ~ +100 m... 328 ft = 99.97 m
    auto s = tr.update(b);
    BOOST_REQUIRE(s);
    BOOST_CHECK(s->vertical_source == VerticalSource::derived);
    // Different rung (baro after geom): not derived.
    Track c = mk(1, 2000, 2 * kM100North, 0);
    c.alt_geom_ft.reset();
    c.alt_baro_ft = 11000;
    s = tr.update(c);
    BOOST_REQUIRE(s);
    BOOST_CHECK(s->velocity_source == VelocitySource::derived);
    BOOST_CHECK(s->vertical_source == VerticalSource::assumed_zero);
}

BOOST_AUTO_TEST_CASE(vertical_assumed_zero)
{
    Track t = mk();
    t.ground_speed_kt = 10.0f;
    t.track_deg = 10.0f;
    BOOST_CHECK(toEcef(t)->vertical_source == VerticalSource::assumed_zero);
}

BOOST_AUTO_TEST_CASE(error_nacp_table)
{
    Track t = mk();
    t.nac_p = 9;
    t.nic = 8;
    auto s = toEcef(t);
    BOOST_CHECK(s->error_source == ErrorSource::nac_p);
    BOOST_CHECK_CLOSE(s->horizontal_sigma_m, 30.0 / 2.45, 1e-9);
    t.nac_p = 11;
    BOOST_CHECK_CLOSE(toEcef(t)->horizontal_sigma_m, 3.0 / 2.45, 1e-9);
    t.nac_p = 1;
    BOOST_CHECK_CLOSE(toEcef(t)->horizontal_sigma_m, 18520.0 / 2.45, 1e-9);
}

BOOST_AUTO_TEST_CASE(nacp_zero_falls_through_to_nic)
{
    Track t = mk();
    t.nac_p = 0;
    t.nic = 8;
    const auto s = toEcef(t);
    BOOST_CHECK(s->error_source == ErrorSource::nic);
    BOOST_CHECK_CLOSE(s->horizontal_sigma_m, 185.2 / 2.45, 1e-9);
}

BOOST_AUTO_TEST_CASE(error_nic_table)
{
    Track t = mk();
    t.nic = 6;
    BOOST_CHECK_CLOSE(toEcef(t)->horizontal_sigma_m, 1111.0 / 2.45, 1e-9);
    t.nic = 11;
    BOOST_CHECK_CLOSE(toEcef(t)->horizontal_sigma_m, 7.5 / 2.45, 1e-9);
    t.nic = 0;
    BOOST_CHECK(toEcef(t)->error_source == ErrorSource::default_);
}

BOOST_AUTO_TEST_CASE(error_default)
{
    const auto s = toEcef(mk());
    BOOST_CHECK(s->error_source == ErrorSource::default_);
    BOOST_CHECK_EQUAL(s->horizontal_sigma_m, 1852.0);
}

BOOST_AUTO_TEST_CASE(tracker_drop_forgets_and_notifies)
{
    Options o;
    o.derive_velocity = true;
    EcefTracker tr(o);
    std::vector<std::pair<TrackId, TrackerDrop>> got;
    tr.onDropped([&](TrackId i, TrackerDrop r) { got.emplace_back(i, r); });
    tr.update(mk(1, 0, 0, 0));
    tr.drop(idOf(1));
    BOOST_REQUIRE_EQUAL(got.size(), 1u);
    BOOST_CHECK(got[0].second == TrackerDrop::explicit_);
    BOOST_CHECK_EQUAL(tr.size(), 0u);
    BOOST_CHECK(tr.update(mk(1, 1000, kM100North, 0))->velocity_source == VelocitySource::none);
}

BOOST_AUTO_TEST_CASE(tracker_drop_unknown_silent)
{
    EcefTracker tr;
    int n = 0;
    tr.onDropped([&](TrackId, TrackerDrop) { ++n; });
    tr.drop(idOf(99));
    BOOST_CHECK_EQUAL(n, 0);
}

BOOST_AUTO_TEST_CASE(tracker_expire_after_stale)
{
    EcefTracker tr;
    std::vector<TrackerDrop> got;
    tr.onDropped([&](TrackId, TrackerDrop r) { got.push_back(r); });
    const auto t0 = SClock::now();
    tr.update(mk());
    BOOST_CHECK_EQUAL(tr.expire(t0 + std::chrono::seconds(31)), 1u);
    BOOST_REQUIRE_EQUAL(got.size(), 1u);
    BOOST_CHECK(got[0] == TrackerDrop::stale);
    BOOST_CHECK_EQUAL(tr.size(), 0u);
}

BOOST_AUTO_TEST_CASE(tracker_expire_before_stale)
{
    EcefTracker tr;
    int n = 0;
    tr.onDropped([&](TrackId, TrackerDrop) { ++n; });
    const auto t0 = SClock::now();
    tr.update(mk());
    BOOST_CHECK_EQUAL(tr.expire(t0 + std::chrono::seconds(29)), 0u);
    BOOST_CHECK_EQUAL(n, 0);
    BOOST_CHECK_EQUAL(tr.size(), 1u);
}

BOOST_AUTO_TEST_CASE(tracker_onstate_first_and_newer_time)
{
    EcefTracker tr;
    int n = 0;
    tr.onState([&](const EcefState&) { ++n; });
    tr.update(mk(1, 0));
    BOOST_CHECK_EQUAL(n, 1);
    tr.update(mk(1, 500));
    BOOST_CHECK_EQUAL(n, 2);
}

BOOST_AUTO_TEST_CASE(tracker_onstate_keyframe_same_time_silent)
{
    EcefTracker tr;
    int n = 0;
    tr.onState([&](const EcefState&) { ++n; });
    tr.update(mk(1, 0));
    Track k = mk(1, 0);
    k.keyframe = true;
    k.nac_p = 9; // refreshed field is stored but not notified
    tr.update(k);
    BOOST_CHECK_EQUAL(n, 1);
    BOOST_CHECK(tr.get(idOf(1))->error_source == ErrorSource::nac_p);
}

BOOST_AUTO_TEST_CASE(tracker_onstate_every_update_when_configured)
{
    Options o;
    o.notify_only_new_position = false;
    EcefTracker tr(o);
    int n = 0;
    tr.onState([&](const EcefState&) { ++n; });
    tr.update(mk(1, 0));
    tr.update(mk(1, 0));
    BOOST_CHECK_EQUAL(n, 2);
}

BOOST_AUTO_TEST_CASE(tracker_no_rung_not_stored)
{
    EcefTracker tr;
    int n = 0;
    tr.onState([&](const EcefState&) { ++n; });
    Track t = mk();
    t.alt_geom_ft.reset();
    BOOST_CHECK(!tr.update(t));
    BOOST_CHECK_EQUAL(tr.size(), 0u);
    BOOST_CHECK_EQUAL(n, 0);
}

BOOST_AUTO_TEST_CASE(tracker_get_and_size)
{
    EcefTracker tr;
    BOOST_CHECK(!tr.get(idOf(1)));
    tr.update(mk(1, 0));
    tr.update(mk(2, 0));
    BOOST_CHECK_EQUAL(tr.size(), 2u);
    BOOST_REQUIRE(tr.get(idOf(2)));
    BOOST_CHECK(tr.get(idOf(2))->id == idOf(2));
    tr.drop(idOf(1));
    BOOST_CHECK_EQUAL(tr.size(), 1u);
}

BOOST_AUTO_TEST_CASE(tracker_clear_notifies_and_empties)
{
    EcefTracker tr;
    int n = 0;
    tr.onDropped([&](TrackId, TrackerDrop r) { n += r == TrackerDrop::cleared; });
    for (uint32_t i = 1; i <= 3; ++i)
        tr.update(mk(i));
    BOOST_CHECK_EQUAL(tr.clear(), 3u);
    BOOST_CHECK_EQUAL(n, 3);
    BOOST_CHECK_EQUAL(tr.size(), 0u);
}

BOOST_AUTO_TEST_CASE(tracker_clear_quiet)
{
    EcefTracker tr;
    int n = 0;
    tr.onDropped([&](TrackId, TrackerDrop) { ++n; });
    tr.update(mk(1));
    BOOST_CHECK_EQUAL(tr.clear(true), 1u);
    BOOST_CHECK_EQUAL(n, 0);
    BOOST_CHECK_EQUAL(tr.size(), 0u);
}

BOOST_AUTO_TEST_CASE(tracker_after_clear_first_update_again)
{
    Options o;
    o.derive_velocity = true;
    EcefTracker tr(o);
    int n = 0;
    tr.onState([&](const EcefState&) { ++n; });
    tr.update(mk(1, 0, 0, 0));
    tr.clear(true);
    const auto s = tr.update(mk(1, 1000, kM100North, 0));
    BOOST_CHECK(s->velocity_source != VelocitySource::derived);
    BOOST_CHECK_EQUAL(n, 2);
}

BOOST_AUTO_TEST_CASE(tracker_callback_may_drop_reentrantly)
{
    EcefTracker tr;
    int dropped = 0;
    tr.onDropped([&](TrackId, TrackerDrop) { ++dropped; });
    tr.onState([&](const EcefState& s) {
        BOOST_CHECK(tr.get(s.id)); // stored before the callback
        tr.drop(s.id);
        tr.clear();
        tr.expire();
    });
    tr.update(mk(1));
    BOOST_CHECK_EQUAL(dropped, 1);
    BOOST_CHECK_EQUAL(tr.size(), 0u);
}

namespace {
Options pull()
{
    Options o;
    o.collect_changes = true;
    return o;
}
} // namespace

BOOST_AUTO_TEST_CASE(take_first_update_is_new)
{
    EcefTracker tr(pull());
    tr.update(mk(1, 0));
    const auto c = tr.takeChanges();
    BOOST_REQUIRE_EQUAL(c.updated.size(), 1u);
    BOOST_CHECK(c.updated[0].is_new);
    BOOST_CHECK(c.dropped.empty());
}

BOOST_AUTO_TEST_CASE(take_coalesces_to_latest_state)
{
    EcefTracker tr(pull());
    tr.update(mk(1, 0));
    tr.update(mk(1, 500));
    const auto c = tr.takeChanges();
    BOOST_REQUIRE_EQUAL(c.updated.size(), 1u);
    BOOST_CHECK(c.updated[0].is_new);
    BOOST_CHECK(c.updated[0].state.time == at(500));
}

BOOST_AUTO_TEST_CASE(take_second_update_after_take_not_new)
{
    EcefTracker tr(pull());
    tr.update(mk(1, 0));
    tr.takeChanges();
    tr.update(mk(1, 500));
    const auto c = tr.takeChanges();
    BOOST_REQUIRE_EQUAL(c.updated.size(), 1u);
    BOOST_CHECK(!c.updated[0].is_new);
}

BOOST_AUTO_TEST_CASE(take_empty_when_nothing_changed)
{
    EcefTracker tr(pull());
    tr.update(mk(1, 0));
    tr.takeChanges();
    const auto c = tr.takeChanges();
    BOOST_CHECK(c.updated.empty() && c.dropped.empty());
}

BOOST_AUTO_TEST_CASE(take_reports_drops_with_reason)
{
    EcefTracker tr(pull());
    const auto t0 = SClock::now();
    tr.update(mk(1, 0));
    tr.update(mk(2, 0));
    tr.update(mk(3, 0));
    tr.takeChanges();
    tr.drop(idOf(1));
    BOOST_CHECK_EQUAL(tr.expire(t0 + std::chrono::seconds(31)), 2u);
    auto c = tr.takeChanges();
    BOOST_REQUIRE_EQUAL(c.dropped.size(), 3u);
    BOOST_CHECK(c.dropped[0].second == TrackerDrop::explicit_);
    BOOST_CHECK(c.dropped[1].second == TrackerDrop::stale);
    tr.update(mk(5, 0));
    tr.takeChanges();
    tr.clear();
    c = tr.takeChanges();
    BOOST_REQUIRE_EQUAL(c.dropped.size(), 1u);
    BOOST_CHECK(c.dropped[0].second == TrackerDrop::cleared);
    BOOST_CHECK(c.dropped[0].first == idOf(5));
}

BOOST_AUTO_TEST_CASE(take_created_and_dropped_only_in_dropped)
{
    EcefTracker tr(pull());
    tr.update(mk(1, 0));
    tr.drop(idOf(1));
    const auto c = tr.takeChanges();
    BOOST_CHECK(c.updated.empty());
    BOOST_REQUIRE_EQUAL(c.dropped.size(), 1u);
    BOOST_CHECK(c.dropped[0].first == idOf(1));
}

BOOST_AUTO_TEST_CASE(take_empty_without_collect_changes)
{
    EcefTracker tr;
    tr.update(mk(1, 0));
    tr.drop(idOf(1));
    const auto c = tr.takeChanges();
    BOOST_CHECK(c.updated.empty() && c.dropped.empty());
}

BOOST_AUTO_TEST_CASE(tracker_threads_smoke)
{
    EcefTracker tr(pull());
    std::atomic<bool> done{false};
    std::size_t news = 0;
    std::thread feeder([&] {
        for (int i = 0; i < 1000; ++i)
            tr.update(mk(static_cast<uint32_t>(i % 10) + 1, i, 0.0, 0.0));
        done = true;
    });
    std::thread taker([&] {
        for (;;) {
            const bool last = done.load();
            for (const auto& ch : tr.takeChanges().updated)
                news += ch.is_new;
            tr.expire();
            tr.get(idOf(1));
            if (last)
                break;
        }
    });
    feeder.join();
    taker.join();
    for (const auto& ch : tr.takeChanges().updated)
        news += ch.is_new;
    BOOST_CHECK_EQUAL(news, 10u);
    BOOST_CHECK_EQUAL(tr.size(), 10u);
}

BOOST_AUTO_TEST_CASE(nacp_12_and_above_falls_through)
{
    Track t = mk();
    t.nac_p = 12;
    t.nic = 8;
    auto s = toEcef(t);
    BOOST_CHECK(s->error_source == ErrorSource::nic);
    t.nic.reset();
    s = toEcef(t);
    BOOST_CHECK(s->error_source == ErrorSource::default_);
}

BOOST_AUTO_TEST_CASE(older_update_does_not_overwrite)
{
    Options o = pull();
    o.derive_velocity = true;
    EcefTracker tr(o);
    int n = 0;
    tr.onState([&](const EcefState&) { ++n; });
    tr.update(mk(1, 10000, 0, 0));
    tr.takeChanges();
    const auto r = tr.update(mk(1, 5000, kM100North * 50, 0));
    BOOST_REQUIRE(r);
    BOOST_CHECK(r->time == at(5000)); // still returned
    BOOST_CHECK(tr.get(idOf(1))->time == at(10000));
    BOOST_CHECK_EQUAL(n, 1);
    BOOST_CHECK(tr.takeChanges().updated.empty());
    // A repeat of the stored time is still silent; t=11 s derives from t=10 s.
    tr.update(mk(1, 10000, 0, 0));
    BOOST_CHECK_EQUAL(n, 1);
    const auto s = tr.update(mk(1, 11000, kM100North, 0));
    BOOST_REQUIRE(s && s->velocity);
    BOOST_CHECK(s->velocity_source == VelocitySource::derived);
    BOOST_CHECK_CLOSE(s->velocity->z, 100.0, 0.5);
}
