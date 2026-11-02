#include "geo.hpp"

#include <algorithm>
#include <cmath>

namespace skycued {
namespace geo {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kEffectiveRadiusM = 4.0 / 3.0 * kEarthRadiusM;

// Central angle in radians between two points (haversine).
double centralAngleRad(double lat1, double lon1, double lat2, double lon2)
{
    const double p1 = lat1 * kDegToRad;
    const double p2 = lat2 * kDegToRad;
    const double dp = (lat2 - lat1) * kDegToRad;
    const double dl = (lon2 - lon1) * kDegToRad;
    const double s1 = std::sin(dp / 2.0);
    const double s2 = std::sin(dl / 2.0);
    double a = s1 * s1 + std::cos(p1) * std::cos(p2) * s2 * s2;
    a = std::min(1.0, std::max(0.0, a));
    return 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

} // namespace

double greatCircleNm(double lat1, double lon1, double lat2, double lon2)
{
    return centralAngleRad(lat1, lon1, lat2, lon2) * kEarthRadiusM / kMetresPerNm;
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2)
{
    const double p1 = lat1 * kDegToRad;
    const double p2 = lat2 * kDegToRad;
    const double dl = (lon2 - lon1) * kDegToRad;
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    if (x == 0.0 && y == 0.0)
        return 0.0;
    double b = std::atan2(y, x) / kDegToRad;
    if (b < 0.0)
        b += 360.0;
    return b >= 360.0 ? 0.0 : b;
}

double elevationDeg(double rx_lat, double rx_lon, double rx_alt_m, double tgt_lat, double tgt_lon, double tgt_alt_m)
{
    const double d = centralAngleRad(rx_lat, rx_lon, tgt_lat, tgt_lon) * kEarthRadiusM;
    const double dh = tgt_alt_m - rx_alt_m;
    return std::atan2(dh - d * d / (2.0 * kEffectiveRadiusM), d) / kDegToRad;
}

} // namespace geo
} // namespace skycued
