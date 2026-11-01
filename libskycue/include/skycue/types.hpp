#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace skycue {

enum class AddressType : uint8_t { icao = 0, non_icao = 1, tisb_trackfile = 2, other = 3 };
enum class Source : uint8_t { adsb_1090 = 0, uat_978 = 1, tisb = 2, adsr = 3, mlat = 4, other = 5 };
enum class DropReason : uint8_t { timeout = 0, filtered = 1, source_lost = 2, shutdown = 3, stale = 4 };

// A track is identified by its 24-bit address together with the address type:
// the same address can belong to an ICAO aircraft and a TIS-B track file at once.
struct TrackId {
    uint32_t address{};
    AddressType type{AddressType::icao};
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

struct Track {
    TrackId id;
    Source source{Source::other};
    std::chrono::system_clock::time_point position_time{};
    double lat_deg{};
    double lon_deg{};
    bool on_ground{};
    bool keyframe{};
    bool alt_is_geom{}; // the elevation filter used geometric altitude
    std::optional<int32_t> alt_baro_ft, alt_geom_ft;
    std::optional<float> ground_speed_kt, track_deg, vertical_rate_fpm;
    std::optional<std::string> callsign, squawk, registration;
    std::optional<uint8_t> nic, nac_p;
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
