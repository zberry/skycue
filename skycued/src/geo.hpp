#pragma once

namespace skycued {
namespace geo {

// Mean earth radius (IUGG), metres.
constexpr double kEarthRadiusM = 6371008.8;
constexpr double kMetresPerNm = 1852.0;

constexpr double ftToM(double ft) { return ft * 0.3048; }

// Great-circle (haversine) distance between two points in degrees, in
// nautical miles of 1852 m, on a sphere of radius kEarthRadiusM.
double greatCircleNm(double lat1, double lon1, double lat2, double lon2);

// Initial great-circle bearing from point 1 to point 2, degrees clockwise
// from true north, in [0, 360). 0 when the points coincide.
double bearingDeg(double lat1, double lon1, double lat2, double lon2);

// Elevation angle in degrees from the receiver antenna (lat/lon in degrees,
// alt_m = site altitude plus antenna height, metres) to a target (lat/lon in
// degrees, alt_m in metres). Positive above the horizontal, negative below.
//
// Model: spherical earth, with atmospheric refraction approximated by the
// standard 4/3 effective earth radius:
//
//     d     = great-circle ground distance (m, on kEarthRadiusM)
//     dh    = tgt_alt_m - rx_alt_m
//     R_eff = 4/3 * kEarthRadiusM
//     angle = atan2(dh - d^2 / (2 * R_eff), d)
//
// The d^2/(2*R_eff) term is the drop of the curved (refracted) earth below
// the receiver's horizontal plane at distance d. Directly overhead (d = 0)
// the angle is +90 (or -90 for a target below the antenna).
double elevationDeg(double rx_lat, double rx_lon, double rx_alt_m, double tgt_lat, double tgt_lon, double tgt_alt_m);

} // namespace geo
} // namespace skycued
