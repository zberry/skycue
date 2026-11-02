#pragma once

// FIS-B product catalogue (docs/fisb-reference.md §3) and the decoder's
// counters.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skycued {
namespace fisb {

constexpr std::size_t kUplinkBytes = 432;   // post-FEC ground uplink payload
constexpr std::size_t kUplinkHeaderBytes = 8;  // UAT-specific header; application data follows

// How a product's APDU payload is encoded.
enum class ProductFamily {
    twgo,          // Text With Graphic Overlay: 8, 11-17
    global_block,  // 63, 64, 70, 71, 84, 90, 91, 103
    generic_text,  // 413: raw DLAC METAR/TAF/PIREP/WINDS
    other,         // anything else: kept as header + raw bytes and counted
};

constexpr ProductFamily productFamily(uint16_t id)
{
    switch (id) {
    case 8: case 11: case 12: case 13: case 14: case 15: case 16: case 17:
        return ProductFamily::twgo;
    case 63: case 64: case 70: case 71: case 84: case 90: case 91: case 103:
        return ProductFamily::global_block;
    case 413:
        return ProductFamily::generic_text;
    default:
        return ProductFamily::other;
    }
}

// Short display name; "other" for IDs outside the table.
constexpr const char* productName(uint16_t id)
{
    switch (id) {
    case 8: return "NOTAM";
    case 11: return "AIRMET";
    case 12: return "SIGMET";
    case 13: return "SUA";
    case 14: return "G-AIRMET";
    case 15: return "CWA";
    case 16: return "NOTAM-TRA";
    case 17: return "NOTAM-TMOA";
    case 63: return "NEXRAD regional";
    case 64: return "NEXRAD CONUS";
    case 70: return "icing low";
    case 71: return "icing high";
    case 84: return "cloud tops";
    case 90: return "turbulence low";
    case 91: return "turbulence high";
    case 103: return "lightning";
    case 413: return "generic text";
    default: return "other";
    }
}

using TimePoint = std::chrono::system_clock::time_point;

// One report from a product-413 APDU (reference §3, §4; design §10.5).
struct TextRecord {
    enum Kind { metar, taf, pirep, winds, other };
    Kind kind = other;
    std::string station;  // first 3-4 character station token after the type; may be empty
    std::string text;     // the report, trailing whitespace and the LF/RS terminator removed
    TimePoint issued{};   // the report's ddhhmmZ group, else the APDU time
    TimePoint expires{};  // left for the store (Task 4) to set from its expiry rules
    std::optional<uint8_t> period_h;  // winds only: forecast period 6, 12 or 24 h (FD WIND_MATRIX)
};

// One TWGO graphic (reference §6.3, §6.4). points are (lat, lon) degrees.
struct Shape {
    // polyline is not in the design's list; G-AIRMET (product 14) sends
    // geometries 11/12, freezing-level contours.
    enum Geom { polygon, polyline, point, circle, prism };
    Geom geom = polygon;
    std::vector<std::pair<double, double>> points;
    double radius_nm = 0;  // circle and prism
    std::optional<int> alt_low_ft, alt_high_ft;
    bool agl = false;  // altitudes above ground (geometries 4, 8, 9, 12) rather than MSL
};

// One decoded global block (reference §5). The 128 bins are 4 rows of 32,
// ordered W->E then N->S from the block's NW corner (§5.2): bin i covers
// latitude [lat0 - (i/32+1)*dlat, lat0 - (i/32)*dlat) and longitude
// [lon0 + (i%32)*dlon, lon0 + (i%32+1)*dlon). lat0/lon0 are the NW corner
// in degrees (lon0 in -180..180), dlat/dlon one bin's height/width in
// degrees. Bin values are the product's raw level (0-7 for NEXRAD, the
// unmapped run value for the others).
struct RadarBlock {
    uint32_t block = 0;
    double lat0 = 0, lon0 = 0, dlat = 0, dlon = 0;
    std::array<uint8_t, 128> bins{};
};

// Running totals kept by the decoder (one instance per decoder).
struct FisbCounters {
    uint64_t uplinks = 0;               // '+' lines / uplink frames seen
    uint64_t bad_hex = 0;               // line did not hold valid hex
    uint64_t bad_frames = 0;            // wrong size, bad header, or an information frame overran
    uint64_t apdus = 0;                 // APDU headers parsed
    uint64_t bad_apdu = 0;              // APDU header truncated
    uint64_t unsupported_products = 0;  // parsed, but productFamily() == other
    uint64_t skipped_experimental = 0;  // T-opt 1/3 with experimental time options off
    uint64_t segments_open = 0;         // segmented products awaiting more segments (gauge)
    uint64_t segments_timed_out = 0;    // incomplete segment sets dropped
    uint64_t blocks_rejected = 0;       // global blocks that failed to decode
};

}  // namespace fisb
}  // namespace skycued
