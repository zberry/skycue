#pragma once

#include <skycue/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace skycued {

// An ACAS resolution advisory as a decoded DF16 or TC28 reported it.
// How long an advisory is shown after `at`, even when nothing more is received.
inline constexpr std::chrono::seconds kAcasRaLife{30};

struct AcasRaState {
    bool active = false;
    uint16_t ara = 0;  // 14 bits, 0x2000 is bit 1 of the field
    uint8_t rac = 0;
    bool rat = false, mte = false;
    uint8_t tti = 0;  // 0 none, 1 threat by address, 2 threat by altitude, range and bearing
    std::optional<uint32_t> threat_address;
    std::optional<int32_t> threat_alt_ft;
    std::optional<float> threat_range_nm, threat_bearing_deg;
    std::string text;
    std::chrono::system_clock::time_point at{};  // when the advisory was received
};

// Nav mode bits of `nav_modes`.
enum NavModeBit : uint8_t {
    kNavAutopilot = 1u << 0,
    kNavVnav = 1u << 1,
    kNavAltHold = 1u << 2,
    kNavApproach = 1u << 3,
    kNavLnav = 1u << 4,
    kNavTcas = 1u << 5,
};
static_assert(kNavTcas == (1u << (skycue::kNavModeCount - 1)), "NavModeBit and skycue::navModeName differ in count");

// Comm-B (BDS 4,0 / 5,0 / 6,0), target-state and ACAS fields of an observation. A
// separate struct so the Engine can pass them to a snapshot without the raw line.
struct CommBFields {
    std::optional<float> heading_mag_deg, heading_true_deg, tas_kt, mach, roll_deg, track_rate_dps, qnh_mb,
        sel_heading_deg;
    std::optional<uint16_t> ias_kt;
    std::optional<int32_t> sel_alt_ft, sel_alt_fms_ft;
    std::optional<uint8_t> nav_modes;  // NavModeBit mask
    std::optional<AcasRaState> acas_ra;
    std::optional<uint8_t> acas_sl, acas_ri;  // DF0/DF16 sensitivity level, reply information
};

// One report about one aircraft from a source (a readsb or dump978 line),
// before filtering and rate limiting turn it into a skycue::Track.
struct TrackObservation : CommBFields {
    skycue::TrackId id;
    skycue::Source source{skycue::Source::other};
    std::chrono::system_clock::time_point received_time{};               // readsb "now"
    std::optional<std::chrono::system_clock::time_point> position_time;  // now - seen_pos; set with lat/lon
    std::optional<double> lat, lon;                                      // both or neither
    std::optional<bool> on_ground;  // unset: the input did not say
    std::optional<int32_t> alt_baro_ft, alt_geom_ft;
    // A GNSS-minus-baro difference the source reported (signed, positive: GNSS above
    // baro), set only when both altitudes came from one message or record, or by a
    // 1090 TC19 velocity message.
    std::optional<int32_t> geom_minus_baro_ft;
    // When each altitude was measured; absent: at received_time. Carried values of the
    // 1090 decoder (up to 60 s old) set them.
    std::optional<std::chrono::system_clock::time_point> alt_baro_time, alt_geom_time;
    // When geom_minus_baro_ft was measured; absent: at received_time. The 1090 decoder sets
    // it to the TC19's time whenever it emits a difference, so a carried difference ages
    // and enters the fit once; other sources leave it absent.
    std::optional<std::chrono::system_clock::time_point> geom_minus_baro_time;
    std::optional<float> gs_kt, track_deg, baro_rate_fpm, geom_rate_fpm;
    std::optional<std::string> callsign, squawk, registration, emergency, category;
    std::optional<uint8_t> nic, nac_p;
    std::optional<double> rssi;
    std::string raw_line;  // kept for the received recording
    std::string input_id;  // the input that produced it; empty when not known
    // A rebroadcast copy (TIS-B, ADS-R): its position yields to a fresh direct
    // one (TrackTable::apply).
    bool rebroadcast = false;
};

// Where the Engine gets its observations: readsb's JSON port or a replay.
// Threading: lives on the Engine's io_context; callbacks run on its thread.
// Register callbacks before start(). After stop() no callback is invoked.
class ObservationSource {
public:
    virtual ~ObservationSource() = default;
    virtual void onObservation(std::function<void(const TrackObservation&)> handler) = 0;
    // true when the source is (re)connected, false when it is lost (a
    // finished replay counts as lost).
    virtual void onConnection(std::function<void(bool)> handler) = 0;
    // UAT uplinks the source carries (a replay's "uat_uplink" records), with
    // their receive time and payload hex. Most sources have none.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): the default no-op of a virtual whose overrides take the handler by value and move it
    virtual void onUplink(std::function<void(std::chrono::system_clock::time_point, const std::string& hex)>) {}
    // Every raw line the source received, before it is parsed (the raw tap
    // of the hub). Most sources have none.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): the default no-op of a virtual whose overrides take the handler by value and move it
    virtual void onRaw(std::function<void(const uint8_t*, std::size_t)>) {}
    virtual void start() = 0;
    virtual void stop() = 0;
    // Lines or records that did not parse (counted in the input's messages
    // and rejected). Most sources have none.
    virtual uint64_t rejected() const { return 0; }
    // Bytes received (the input's `bytes` counter). 0 when not counted.
    virtual uint64_t bytesRead() const { return 0; }
};

}  // namespace skycued
