#include "decode/cpr.hpp"

#include <algorithm>
#include <cmath>

namespace skycued::decode::cpr {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kScale = 131072.0;  // 2^17
constexpr double kEarthNm = 3440.065;

double posMod(double a, double b) { return a - b * std::floor(a / b); }
double normLon(double lon) { return posMod(lon + 180.0, 360.0) - 180.0; }
double wrapLat(double lat) { return lat >= 270.0 ? lat - 360.0 : lat; }

// Angular difference in degrees, folded to [0, 180].
double lonDiff(double a, double b) { return std::fabs(posMod(a - b + 180.0, 360.0) - 180.0); }

struct PairLat {
    double even, odd;  // raw latitudes of the two messages
};

// Latitude index j and the two latitudes. zone: 360 airborne, 90 surface.
PairLat pairLat(const Msg& even, const Msg& odd, double zone) {
    const double lat0 = even.lat17 / kScale, lat1 = odd.lat17 / kScale;
    const double j = std::floor(59.0 * lat0 - 60.0 * lat1 + 0.5);
    return {(zone / 60.0) * (posMod(j, 60.0) + lat0), (zone / 59.0) * (posMod(j, 59.0) + lat1)};
}

// Longitude of the newer message given its final latitude's NL.
double pairLon(const Msg& even, const Msg& odd, bool newer_is_odd, double zone, int nlv) {
    const double lon0 = even.lon17 / kScale, lon1 = odd.lon17 / kScale;
    const int ni = std::max(nlv - (newer_is_odd ? 1 : 0), 1);
    const double m = std::floor(lon0 * (nlv - 1) - lon1 * nlv + 0.5);
    return (zone / ni) * (posMod(m, ni) + (newer_is_odd ? lon1 : lon0));
}

// The candidate raw + 90*k (k in -2..1, kept within +-90) closest to target.
double nearestLat(double raw, double target) {
    double best = raw, bd = 1e9;
    for (int k = -2; k <= 1; ++k) {
        const double c = raw + 90.0 * k;
        if (c < -90.0 || c > 90.0) continue;
        const double d = std::fabs(c - target);
        if (d < bd) {
            bd = d;
            best = c;
        }
    }
    return best;
}

}  // namespace

int nl(double lat_deg) {
    const double lat = std::fabs(lat_deg);
    if (lat == 0.0) return 59;  // the closed form gives 60; the standard fixes 59
    if (lat == 87.0) return 2;
    if (lat > 87.0) return 1;
    const double a = 1.0 - std::cos(kPi / 30.0);  // NZ = 15
    const double c = std::cos(kPi / 180.0 * lat);
    return static_cast<int>(std::floor(2.0 * kPi / std::acos(1.0 - a / (c * c))));
}

std::optional<Position> globalAirborne(const Msg& even, const Msg& odd, bool newer_is_odd) {
    const PairLat r = pairLat(even, odd, 360.0);
    const double le = wrapLat(r.even), lo = wrapLat(r.odd);
    if (nl(le) != nl(lo)) return std::nullopt;
    const double lat = newer_is_odd ? lo : le;
    return Position{lat, normLon(pairLon(even, odd, newer_is_odd, 360.0, nl(lat)))};
}

std::optional<Position> globalSurface(const Msg& even, const Msg& odd, bool newer_is_odd,
                                      const Position& ref) {
    const PairLat r = pairLat(even, odd, 90.0);
    // Raw latitudes lie in [0, 90): shift the newer one to the quadrant
    // nearest the reference, then the other one to sit beside it.
    const double lat = nearestLat(newer_is_odd ? r.odd : r.even, ref.lat);
    const double other = nearestLat(newer_is_odd ? r.even : r.odd, lat);
    if (nl(lat) != nl(other)) return std::nullopt;
    const double lon = pairLon(even, odd, newer_is_odd, 90.0, nl(lat));
    double best = lon, bd = 1e9;
    for (int k = -2; k <= 1; ++k) {
        const double c = lon + 90.0 * k;
        const double d = lonDiff(c, ref.lon);
        if (d < bd) {
            bd = d;
            best = c;
        }
    }
    return Position{lat, normLon(best)};
}

std::optional<Position> local(const Msg& m, const Position& ref, bool surface) {
    const double zone = surface ? 90.0 : 360.0;
    const double dlat = zone / (m.odd ? 59.0 : 60.0);
    const double lat_c = m.lat17 / kScale, lon_c = m.lon17 / kScale;
    const double j = std::floor(ref.lat / dlat) + std::floor(0.5 + posMod(ref.lat, dlat) / dlat - lat_c);
    const double lat = dlat * (j + lat_c);
    const double dlon = zone / std::max(nl(lat) - (m.odd ? 1 : 0), 1);
    const double mm = std::floor(ref.lon / dlon) + std::floor(0.5 + posMod(ref.lon, dlon) / dlon - lon_c);
    return Position{wrapLat(lat), normLon(dlon * (mm + lon_c))};
}

double distanceNm(const Position& a, const Position& b) {
    const double rad = kPi / 180.0;
    const double dlat = (b.lat - a.lat) * rad, dlon = (b.lon - a.lon) * rad;
    const double h = std::sin(dlat / 2) * std::sin(dlat / 2) +
                     std::cos(a.lat * rad) * std::cos(b.lat * rad) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2.0 * kEarthNm * std::asin(std::min(1.0, std::sqrt(h)));
}

}  // namespace skycued::decode::cpr
