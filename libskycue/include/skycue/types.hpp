#pragma once

// skycue track types: what a Client hands to its callbacks. See
// docs/libskycue.md for the API guide and docs/protocol.md for the wire format.
//
// Enumerations: the values below are the ones protocol version 1 defines. The
// decoder passes the wire byte through unchanged, so a newer sender may deliver
// a value outside this set (a switch over these enums can fall through every
// case). Treat any value you do not recognise as `other`.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace skycue {

// What kind of 24-bit address TrackId::address is. Part of a track's identity.
enum class AddressType : uint8_t {
    icao = 0,           // an ICAO aircraft address (Mode S / ADS-B transponder)
    non_icao = 1,       // an anonymous or self-assigned address, not an ICAO one
    tisb_trackfile = 2, // a TIS-B track-file number assigned by ground radar, not an aircraft address
    other = 3,          // any other kind; also how to treat values this version does not know
};

// Which kind of surveillance the sender's latest data for a track came from.
enum class Source : uint8_t {
    adsb_1090 = 0, // ADS-B on 1090 MHz, broadcast by the aircraft itself
    uat_978 = 1,   // UAT on 978 MHz. skycued reports traffic from its 978 MHz
                   // receiver this way, although readsb, which bridges it in,
                   // labels it ADS-R (adsr_*).
    tisb = 2,      // TIS-B: ground radar targets rebroadcast by a ground station
    adsr = 3,      // ADS-R: rebroadcast between 1090 and 978 by a ground
                   // station. Defined by the protocol; the current skycued
                   // reports readsb's adsr_* traffic as uat_978 instead.
    mlat = 4,      // multilateration: position computed from arrival times
    other = 5,     // Mode S without position, ADS-C, unknown; also how to treat
                   // values this version does not know
};

// Why a track left the table (Client::onTrackDropped). The first four arrive
// from the sender in a TrackDrop; `stale` and the library's own `source_lost`
// are decided by the library. After any drop, forget the track; if it comes
// back it arrives as a fresh onTrackUpdate.
enum class DropReason : uint8_t {
    timeout = 0,     // the sender stopped hearing the aircraft (its TRACK_TIMEOUT_S passed)
    filtered = 1,    // the sender still hears it, but its filters (range,
                     // elevation, block list) now exclude it
    source_lost = 2, // either the sender lost its own upstream (the radio
                     // decoder) and its tracks aged out, or the library lost
                     // the sender: connection closed, liveness timeout or (udp)
                     // silence. Data may resume later.
    shutdown = 3,    // the sender is exiting cleanly
    stale = 4,       // library-side: no keyframe update for the track for more
                     // than two keyframe intervals, i.e. it was missing from
                     // the sender's periodic full refreshes (a lost drop, say)
};

// A track is identified by its 24-bit address together with the address type:
// the same address can belong to an ICAO aircraft and a TIS-B track file at once.
struct TrackId {
    uint32_t address{};                  // the 24-bit address, in the low 24 bits
    AddressType type{AddressType::icao}; // see AddressType
};

inline bool operator==(const TrackId& a, const TrackId& b)
{
    return a.address == b.address && a.type == b.type;
}
inline bool operator!=(const TrackId& a, const TrackId& b) { return !(a == b); }
inline bool operator<(const TrackId& a, const TrackId& b)
{
    if (a.address != b.address)
        return a.address < b.address;
    return static_cast<uint8_t>(a.type) < static_cast<uint8_t>(b.type);
}

// The full state of one track, as the sender last knew it. Every update
// carries every field the sender knows, so a consumer replaces its copy rather
// than merging: an optional that is nullopt now but was set before means the
// sender no longer has that value.
//
// std::nullopt on any optional field means "not known / not reported". Units
// are fixed: degrees, feet, knots, feet per minute.
struct Track {
    TrackId id;
    Source source{Source::other}; // where the sender's latest data came from
    // When the position below was measured, on the sender's clock, UTC (not
    // when the frame was sent or received). Millisecond resolution.
    std::chrono::system_clock::time_point position_time{};
    double lat_deg{}; // WGS-84 latitude, degrees, north positive, [-90, 90]; resolution 1e-7
    double lon_deg{}; // WGS-84 longitude, degrees, east positive, [-180, 180]; resolution 1e-7
    // The aircraft reports itself on the ground. alt_baro_ft is then
    // normally nullopt (the report carries "ground" instead of an altitude).
    bool on_ground{};
    // Part of a periodic full refresh (or of the burst a new TCP connection
    // gets) rather than a reaction to a change; the content means exactly
    // what a normal update's does. Keyframe updates are what keep a track
    // from going stale.
    bool keyframe{};
    // The sender's elevation filter used alt_geom_ft for this track; false:
    // it used alt_baro_ft (or the aircraft is on the ground, or had no altitude).
    bool alt_is_geom{};
    std::optional<int32_t> alt_baro_ft; // barometric (pressure) altitude, feet, standard pressure
    std::optional<int32_t> alt_geom_ft; // geometric (GNSS) altitude, feet
    std::optional<float> ground_speed_kt;   // speed over ground, knots
    std::optional<float> track_deg;         // track over ground, degrees true, [0, 360)
    std::optional<float> vertical_rate_fpm; // climb positive, feet per minute; barometric
                                            // rate when known, else geometric
    std::optional<std::string> callsign;     // flight id, at most 8 bytes, trailing spaces removed
    std::optional<std::string> squawk;       // Mode A code, normally 4 octal digits, e.g. "7000"
    std::optional<std::string> registration; // tail number (e.g. "N123AB"), at most 16 bytes
    std::optional<uint8_t> nic;   // Navigation Integrity Category as reported (0-11, higher is better)
    std::optional<uint8_t> nac_p; // Navigation Accuracy Category for position (0-11, higher is better)
    // Present exactly when alt_geom_ft is an estimate rather than a reported value:
    // its expected error, feet (saturates at 65535). Absent: reported, or no geometric altitude.
    std::optional<uint16_t> alt_geom_uncertainty_ft;
};

} // namespace skycue

namespace std {
template <> struct hash<skycue::TrackId> {
    size_t operator()(const skycue::TrackId& id) const noexcept
    {
        return std::hash<uint64_t>{}((static_cast<uint64_t>(static_cast<uint8_t>(id.type)) << 32) | id.address);
    }
};
} // namespace std
