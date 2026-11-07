#pragma once

// Compact Position Reporting: pure position math, no per-aircraft state (the
// caller owns message pairing and range rules). Equations: ICAO Doc 9871 /
// Annex 10 Vol IV, as laid out in The 1090 MHz Riddle (Junzi Sun), position
// chapter. 17-bit lat/lon fields as returned by decodeCpr().

#include <cstdint>
#include <optional>

namespace skycued::decode::cpr {

// Number of longitude zones at a latitude: 59 at the equator, 1 beyond +-87.
int nl(double lat_deg);

struct Position {
    double lat, lon;
};
struct Msg {
    uint32_t lat17, lon17;
    bool odd;
};

// Global decode from one even and one odd airborne message. The result is the
// position of the newer message. nullopt: the two latitudes fall in different
// NL zones (the pair straddles a boundary and must not be combined).
std::optional<Position> globalAirborne(const Msg& even, const Msg& odd, bool newer_is_odd);

// Surface variant (90 degree zones). A surface pair is ambiguous between four
// quadrants, resolved by the reference position.
std::optional<Position> globalSurface(const Msg& even, const Msg& odd, bool newer_is_odd,
                                      const Position& ref);

// Local decode from a single message and a reference within half a zone
// (about 180 NM airborne, 45 NM surface). Never fails; a distant reference
// yields a plausible but wrong position, so the caller applies a range rule.
std::optional<Position> local(const Msg& m, const Position& ref, bool surface);

// Great-circle distance in nautical miles.
double distanceNm(const Position& a, const Position& b);

}  // namespace skycued::decode::cpr
