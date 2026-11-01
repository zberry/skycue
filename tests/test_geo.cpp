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
