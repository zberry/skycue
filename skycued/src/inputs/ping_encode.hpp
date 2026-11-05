#pragma once

// TrackObservation -> what a pingStation 3 sends: the inverses of
// parsePingDatagram (ping_json.hpp) and mapCat021Record (cat021_map.hpp),
// for skycue-pingsim and the tests. Our own encoders, from the receiver's interface document and the
// CAT021 reference. Pure.

#include "asterix/cat021.hpp"
#include "observation.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace skycued {

// A bare aircraft object (no "aircraft" array), "utcSync":1 and "timeStamp"
// = stamp. Position, altitudes (baro as altitudeType 0, geometric as the
// secondary altitude, or as the primary when there is no baro), velocity,
// heading, one vertical rate (baro preferred), callsign, squawk, emitterType
// (A0..C7) and trafficSource; `detail` carries addressQualifier,
// emergencyStatus, navIntegrity, navPositionAccuracy and the vertical rate's
// source, and airGroundState (2 on the ground, 0 airborne) only when
// on_ground is set. A TIS-B source is addressQualifier 2 (ICAO address) or
// 3. Registration and rssi have no field. Never throws but bad_alloc.
std::string pingJsonFromObservation(const TrackObservation& obs, std::chrono::system_clock::time_point stamp);

// {"status":{...}} as pingsim sends it: guid "0000000000000001", version
// 1.0.0, gpsStatus 3, receiverStatus 0, timeStamp = stamp, the position.
std::string pingStatusJson(double lat, double lon, std::chrono::system_clock::time_point stamp);

// One CAT021 record: 010, 040 (ATP 0 ICAO, 3 non-ICAO, 4 otherwise; ARC
// 25 ft; ext 1 with GBS only when on_ground is set), 131, 080, 073 = time of
// day of `stamp`, 090 from nic/nac_p, 210 LTT (2 for adsb_1090, 1 for
// uat_978, else 0), 070, 145 = round(alt_baro/25) (absent on the ground),
// 140 from alt_geom_ft, 155/157, 160, 170, 020 for the categories
// category020 maps back (A1-A7, B1-B4, B6, B7, C1-C5; none for A0, B0, B5,
// C0, C6, C7, D*), 200 PS from the emergency. A value that does not fit its
// item (a squawk that is not 4 octal digits, an altitude past the item's
// range, NIC 16, ...) leaves the item out, so encodeRecord never throws on
// the result.
asterix::Cat021Record cat021FromObservation(const TrackObservation& obs, uint8_t sac, uint8_t sic,
                                            std::chrono::system_clock::time_point stamp);

}  // namespace skycued
