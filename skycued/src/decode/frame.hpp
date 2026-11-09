#pragma once

// A single Mode S frame and its pure field extraction. No per-aircraft state:
// the decoders here read one frame and return what it says. Layouts follow
// ICAO Annex 10 Vol IV as summarised in The 1090 MHz Riddle (Junzi Sun).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "decode/altitude.hpp"
#include "decode/field56.hpp"

namespace skycued::decode {

enum class FrameKind { df0, df4, df5, df11, df16, df17, df18, df20, df21, unknown };

struct Frame {  // a 7- or 14-byte Mode S frame, validated for length/DF
    std::vector<uint8_t> bytes;
    FrameKind kind = FrameKind::unknown;
    int df = -1;
    // Big-endian bit field, first = 0..111. Throws std::out_of_range when the
    // field leaves the frame or count > 32.
    uint32_t bits(unsigned first, unsigned count) const;
    // The 56 bits starting at frame bit `first` (0-based; 32 for ME/MB/MV) as a
    // uint64_t with the first bit most significant. Throws std::out_of_range.
    uint64_t field56(unsigned first) const;
};

// nullopt: length/DF mismatch or DF not listed.
std::optional<Frame> classify(const std::vector<uint8_t>& bytes);

// Extended squitter (DF17/18) fields. Each returns nullopt/empty/-1 for a
// frame that is not a 14-byte DF17/18 or whose type code does not fit.
struct Identification {
    std::string callsign;
    std::string category;  // "A3" style
};
std::optional<Identification> decodeIdentification(const Frame& f);  // TC 1..4; invalid char -> nullopt

struct Velocity {
    std::optional<float> gs_kt, track_deg;
    std::optional<int32_t> rate_fpm;
    bool rate_is_geom = false;
    std::optional<int32_t> gnss_baro_diff_ft;
    // ST3/4 only: magnetic heading and airspeed (IAS or TAS by the type bit).
    std::optional<float> heading_mag_deg;
    std::optional<uint16_t> ias_kt, tas_kt;
};
std::optional<Velocity> decodeVelocity(const Frame& f);  // TC19 ST1-4 (ST3/4: heading/airspeed, no gs/track)

// TC29 target state and status, subtype 1 (version 2). Subtype 0 -> nullopt.
struct TargetState {
    std::optional<int32_t> sel_alt_ft, sel_alt_fms_ft;  // by the altitude type bit
    std::optional<float> qnh_mb, sel_heading_deg;
    std::optional<uint8_t> nac_p;
    bool modes_valid = false;
    bool autopilot = false, vnav = false, althold = false, approach = false, tcas = false, lnav = false;
};
std::optional<TargetState> decodeTargetState(const Frame& f);

struct SurfaceMovement {
    std::optional<float> gs_kt, track_deg;
};
SurfaceMovement decodeSurfaceMovement(const Frame& f);  // TC5-8

struct CprFields {
    bool odd;
    uint32_t lat17, lon17;
    bool surface;
};
std::optional<CprFields> decodeCpr(const Frame& f);  // TC5-8, 9-18, 20-22; else nullopt

std::optional<std::string> decodeEmergency(const Frame& f);  // TC28 ST1: "none".."downed_aircraft"; 7 -> nullopt
std::optional<std::string> decodeSquawkTc28(const Frame& f);

struct OperationalStatus {
    int version;
    bool nic_a;
    bool nic_c;
    std::optional<uint8_t> nac_p;
    bool surface;
};
std::optional<OperationalStatus> decodeOperationalStatus(const Frame& f);  // TC31

// Navigation integrity category from the type code and the supplement bits.
// nic_b_or_c is NIC supplement-B for airborne and NIC supplement-C for surface
// position types. Version 2 uses the full TC/A/B/C table, version 1 only the
// single supplement bit (passed as nic_a), version 0 none. nullopt for a type
// code that carries no position.
std::optional<uint8_t> nicFor(int tc, int version, bool nic_a, bool nic_b_or_c, bool surface);

int typeCode(const Frame& f);                     // ME bits 0..4; -1 when not DF17/18
std::string decodeCallsignIdent(const Frame& f);  // AIS 6-bit, trimmed; "" when any char invalid

// Mode S (non-ES) fields
int flightStatus(const Frame& f);          // DF4/5/20/21 FS (-1 otherwise)
bool verticalStatusGround(const Frame& f);  // DF0/16 VS == 1
int capability(const Frame& f);            // DF11/17 CA; DF18: control field (-1 otherwise)

}  // namespace skycued::decode
