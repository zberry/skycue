#define BOOST_TEST_MODULE skycue_ecef
#include <boost/test/unit_test.hpp>

#include <skycue/ecef.hpp>

#include <cmath>

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

BOOST_AUTO_TEST_CASE(published_point)
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
