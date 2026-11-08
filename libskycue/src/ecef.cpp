// ECEF geodesy core: WGS-84 geodetic and NED conversions, look angles and
// propagation. See include/skycue/ecef.hpp.

#include <skycue/ecef.hpp>

#include <cmath>

namespace skycue::geo {

namespace {

constexpr double kA = 6378137.0;                    // WGS-84 semi-major axis, m
constexpr double kF = 1.0 / 298.257223563;          // WGS-84 flattening
constexpr double kE2 = kF * (2.0 - kF);             // first eccentricity squared
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

struct Basis {
    Vec3 n, e, d; // unit vectors north, east, down in ECEF
};

Basis basisAt(double lat_rad, double lon_rad)
{
    const double sp = std::sin(lat_rad), cp = std::cos(lat_rad);
    const double sl = std::sin(lon_rad), cl = std::cos(lon_rad);
    return {{-sp * cl, -sp * sl, cp}, {-sl, cl, 0.0}, {-cp * cl, -cp * sl, -sp}};
}

} // namespace

Vec3 geodeticToEcef(double lat_deg, double lon_deg, double h_m)
{
    const double lat = lat_deg * kDeg, lon = lon_deg * kDeg;
    const double sp = std::sin(lat), cp = std::cos(lat);
    const double n = kA / std::sqrt(1.0 - kE2 * sp * sp);
    return {(n + h_m) * cp * std::cos(lon), (n + h_m) * cp * std::sin(lon), (n * (1.0 - kE2) + h_m) * sp};
}

Vec3 nedToEcef(double lat_deg, double lon_deg, double vn, double ve, double vd)
{
    const Basis b = basisAt(lat_deg * kDeg, lon_deg * kDeg);
    return {vn * b.n.x + ve * b.e.x + vd * b.d.x, vn * b.n.y + ve * b.e.y + vd * b.d.y,
            vn * b.n.z + ve * b.e.z + vd * b.d.z};
}

LookAngles lookFrom(const Vec3& sensor, const Vec3& target)
{
    // Geodetic latitude and longitude of the sensor, only to orient the ENU
    // axes (height is not needed). Iterate lat = atan2(z + e2 N sin(lat), p);
    // it converges to well under a nanoradian in a few steps.
    const double p = std::hypot(sensor.x, sensor.y);
    const double lon = std::atan2(sensor.y, sensor.x);
    double lat = std::atan2(sensor.z, p * (1.0 - kE2));
    for (int i = 0; i < 8; ++i) {
        const double sp = std::sin(lat);
        const double n = kA / std::sqrt(1.0 - kE2 * sp * sp);
        lat = std::atan2(sensor.z + kE2 * n * sp, p);
    }
    const Basis b = basisAt(lat, lon);
    const double dx = target.x - sensor.x, dy = target.y - sensor.y, dz = target.z - sensor.z;
    const double north = dx * b.n.x + dy * b.n.y + dz * b.n.z;
    const double east = dx * b.e.x + dy * b.e.y + dz * b.e.z;
    const double up = -(dx * b.d.x + dy * b.d.y + dz * b.d.z);
    const double horiz = std::hypot(east, north);

    LookAngles out;
    out.range_m = std::sqrt(horiz * horiz + up * up);
    out.azimuth_deg = std::atan2(east, north) / kDeg;
    if (out.azimuth_deg < 0.0)
        out.azimuth_deg += 360.0;
    if (out.azimuth_deg >= 360.0) // a tiny negative angle rounds up to 360
        out.azimuth_deg = 0.0;
    out.elevation_deg = std::atan2(up, horiz) / kDeg;
    return out;
}

Vec3 propagate(const EcefState& s, std::chrono::system_clock::time_point to)
{
    if (!s.velocity)
        return s.position;
    const double dt = std::chrono::duration<double>(to - s.time).count();
    return {s.position.x + s.velocity->x * dt, s.position.y + s.velocity->y * dt,
            s.position.z + s.velocity->z * dt};
}

} // namespace skycue::geo
