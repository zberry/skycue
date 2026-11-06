#define BOOST_TEST_MODULE skycued_geo
#include <boost/test/unit_test.hpp>

#include "geo.hpp"

namespace geo = skycued::geo;

BOOST_AUTO_TEST_CASE(zero_distance)
{
    BOOST_TEST(geo::greatCircleNm(38.9, -77.0, 38.9, -77.0) == 0.0);
}

BOOST_AUTO_TEST_CASE(one_degree_lat_is_60nm)
{
    const double d = geo::greatCircleNm(10.0, 20.0, 11.0, 20.0);
    BOOST_TEST(d > 59.9);
    BOOST_TEST(d < 60.1);
}

BOOST_AUTO_TEST_CASE(dc_to_nyc_about_179nm)
{
    // The plan said 177-181 nm, but for these rounded coordinates the great
    // circle is 175.57 nm (checked independently with the spherical law of
    // cosines); the city centres proper are ~176.9 nm apart.
    const double d = geo::greatCircleNm(38.9, -77.0, 40.7, -74.0);
    BOOST_TEST(d >= 175.0);
    BOOST_TEST(d <= 176.0);
}

BOOST_AUTO_TEST_CASE(elevation_overhead_is_90)
{
    const double e = geo::elevationDeg(38.9, -77.0, 0.0, 38.9, -77.0, 10000.0);
    BOOST_TEST(e >= 89.9);
    BOOST_TEST(e <= 90.0);
}

BOOST_AUTO_TEST_CASE(elevation_horizon_negative_at_200nm_low)
{
    // 200 nm due north: 200/60.04 degrees of latitude.
    const double dlat = 200.0 / geo::greatCircleNm(0.0, 0.0, 1.0, 0.0);
    const double e = geo::elevationDeg(38.0, -77.0, 0.0, 38.0 + dlat, -77.0, geo::ftToM(1000.0));
    BOOST_TEST(e < 0.0);
}

BOOST_AUTO_TEST_CASE(elevation_at_30nm_35000ft_about_11deg)
{
    const double dlat = 30.0 / geo::greatCircleNm(0.0, 0.0, 1.0, 0.0);
    const double e = geo::elevationDeg(38.0, -77.0, 0.0, 38.0 + dlat, -77.0, geo::ftToM(35000.0));
    BOOST_TEST(e >= 10.0);
    BOOST_TEST(e <= 12.0);
}

BOOST_AUTO_TEST_CASE(antenna_height_changes_angle_monotonically)
{
    const double dlat = 30.0 / geo::greatCircleNm(0.0, 0.0, 1.0, 0.0);
    const double tgt_alt = geo::ftToM(10000.0);
    double prev = 91.0;
    for (double rx_alt : {0.0, 10.0, 100.0, 500.0, 1000.0}) {
        const double e = geo::elevationDeg(38.0, -77.0, rx_alt, 38.0 + dlat, -77.0, tgt_alt);
        BOOST_TEST(e < prev); // a higher antenna sees the same target lower
        prev = e;
    }
    // ... and a target below the antenna lies below the horizontal.
    BOOST_TEST(geo::elevationDeg(38.0, -77.0, 4000.0, 38.0 + dlat, -77.0, tgt_alt) < 0.0);
}

BOOST_AUTO_TEST_CASE(ft_to_m)
{
    BOOST_TEST(geo::ftToM(1000.0) == 304.8, boost::test_tools::tolerance(1e-9));
}

BOOST_AUTO_TEST_CASE(bearing_cardinal_directions)
{
    namespace tt = boost::test_tools;
    // Short distances (0.01 deg), so the parallel and the great circle agree to well under 0.1 deg.
    BOOST_TEST(geo::bearingDeg(38.9, -77.0, 38.91, -77.0) == 0.0, tt::tolerance(1e-9));
    BOOST_TEST(geo::bearingDeg(38.9, -77.0, 38.9, -76.99) == 90.0, tt::tolerance(0.1 / 90.0));
    BOOST_TEST(geo::bearingDeg(38.9, -77.0, 38.89, -77.0) == 180.0, tt::tolerance(1e-9));
    BOOST_TEST(geo::bearingDeg(38.9, -77.0, 38.9, -77.01) == 270.0, tt::tolerance(0.1 / 270.0));
}

BOOST_AUTO_TEST_CASE(bearing_is_in_0_360)
{
    // Just west of north: close to 360, never negative, never 360 itself.
    const double b = geo::bearingDeg(38.9, -77.0, 39.9, -77.0001);
    BOOST_TEST(b > 359.9);
    BOOST_TEST(b < 360.0);
    BOOST_TEST(geo::bearingDeg(38.9, -77.0, 38.9, -77.0) == 0.0);  // same point
    // Same point written with either zero: no bearing, and not 180 from atan2(+-0, -0).
    BOOST_TEST(geo::bearingDeg(0.0, 0.0, -0.0, -0.0) == 0.0);
    BOOST_TEST(geo::bearingDeg(-0.0, -0.0, 0.0, 0.0) == 0.0);
    BOOST_TEST(geo::bearingDeg(-0.0, 10.0, 0.0, 10.0) == 0.0);
}
