#pragma once

// A UAT (978 MHz) ADS-B downlink frame and its pure field extraction. No
// per-aircraft state, no clock, no I/O.
//
// Layout (the public UAT ADS-B description, RTCA DO-282B / the FAA UAT ADS-B
// message summary; bytes 0-based, bits MSB first, bit numbers 0-based over the
// whole frame):
//   byte 0        payload type (bits 0-4), address qualifier (bits 5-7)
//   bytes 1-3     address (bits 8-31)
//   state vector  bytes 4-16 (bits 32-135):
//     32-54 latitude (23 bits), 55-78 longitude (24 bits), both x 360/2^24
//     79 altitude type (0 barometric, 1 geometric), 80-91 altitude (12 bits)
//     92-95 NIC, 96-97 A/G state (0 airborne, 1 airborne supersonic, 2 ground)
//     airborne: 98 N/S sign (1 = south), 99-108 N/S (10 bits), 109 E/W sign
//       (1 = west), 110-119 E/W (10 bits), 120 vertical-rate source (1 =
//       geometric), 121 sign (1 = down), 122-130 vertical rate (9 bits)
//     ground:   98-108 speed (11 bits), 109-110 track type (1 = true track; 0 n/a, 2 magnetic heading, 3 true
//       heading give no track; golden-arbitrated (Task 5a)),
//       111-119 track/heading (9 bits, x 360/512)
//   mode status   bytes 17-28 (types 1 and 3): emitter category + eight base-40
//     characters in three 16-bit groups (bits 136-183), emergency (3 bits at
//     184), NACp (4 bits at 200), CSID (bit 213)
//   AUX SV        bytes 29-33 (types 1, 2, 5, 6): bits 232-243 secondary altitude
// Every field is cited next to its constant in uat_frame.cpp; the ones marked
// "golden-arbitrated" are fixed by the dump978-fa golden comparison (plan Task 5).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "skycue/types.hpp"

namespace skycued::decode::uat {

enum class AddressQualifier : uint8_t {
    adsb_icao = 0,
    adsb_selfassigned = 1,
    tisb_adsr_icao = 2,
    tisb_trackfile = 3,
    surface_vehicle = 4,
    fixed_beacon = 5,
    adsr_nonicao = 6,
    reserved = 7
};

struct QualifierMap {
    skycue::AddressType type;
    skycue::Source source;
    bool rebroadcast;
};
std::optional<QualifierMap> mapQualifier(AddressQualifier q);  // nullopt for reserved; decoder spec §6

struct UatFrame {
    // 18 or 34 bytes when made by parseDownlinkLine (the intended constructor path);
    // accessors and decoders return defaults, never read out of bounds, on shorter ones.
    std::vector<uint8_t> bytes;
    std::optional<double> rssi_dbfs;     // from ";rssi="
    std::optional<int> rs_errors;        // from ";rs="
    uint8_t payload_type() const;        // bits 0-4
    AddressQualifier qualifier() const;  // bits 5-7
    uint32_t address() const;            // 24 bits
    bool lengthMatchesType() const;      // 18 <=> type 0; 34 <=> 1..10
    bool hasMs() const;                  // types 1, 3 (and a matching length)
    bool hasAux() const;                 // types 1, 2, 5, 6 (and a matching length)
};

// "-HEX;rssi=-12.0;rs=1;t=..." (the '-' optional, unknown keys ignored).
// nullopt: not hex, odd length, or length not 18/34 bytes.
std::optional<UatFrame> parseDownlinkLine(std::string_view line);

enum class AltType : uint8_t { baro, geometric };

struct StateVector {
    std::optional<double> lat, lon;  // absent when both raw fields are 0 and nic == 0
    AltType alt_type = AltType::baro;
    std::optional<int32_t> altitude_ft;  // (raw - 1) * 25 - 1000; raw 0 = unavailable
    uint8_t nic = 0;
    enum class Ag : uint8_t { airborne_subsonic, airborne_supersonic, ground, reserved };
    Ag ag = Ag::airborne_subsonic;
    std::optional<float> gs_kt, track_deg;  // airborne from N/S, E/W (x4 when supersonic); ground only when the track type says track
    std::optional<int32_t> vrate_fpm;       // 64 fpm steps, sign and source bit
    std::optional<bool> vrate_geometric;
};

struct ModeStatus {
    std::string emitter_category;  // "A0".."D7" from value / 8, % 8; empty when 0 (no info)
    std::optional<std::string> callsign;  // CSID 1
    std::optional<std::string> squawk;    // CSID 0 and four octal digits
    std::optional<std::string> emergency; // 0..6 as decoder spec §5.3
    std::optional<uint8_t> nac_p;
};

StateVector decodeStateVector(const UatFrame&);
std::optional<ModeStatus> decodeModeStatus(const UatFrame&);  // nullopt unless hasMs()
std::optional<int32_t> decodeAuxSv(const UatFrame&);          // secondary altitude in feet; nullopt unless hasAux() or raw 0

}  // namespace skycued::decode::uat
