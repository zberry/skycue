#pragma once

// UAT ground uplink frame (432 bytes after FEC): the 8-byte UAT header, the
// information frames in the 424-byte application data, and the dump978 text
// line that carries it. Layouts: docs/fisb-reference.md §1.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace skycued {
namespace fisb {

struct UplinkHeader {
    double lat_deg = 0, lon_deg = 0;  // ground station; decoded even when !position_valid
    bool position_valid = false;      // always 0 in practice: do not gate on it
    bool utc_coupled = false;
    bool app_data_valid = false;      // 0: ignore the application data
    uint8_t slot_id = 0;              // 5 bits
    uint8_t tisb_site_id = 0;         // 4 bits
};

// nullopt when n < 8.
std::optional<UplinkHeader> parseUplinkHeader(const uint8_t* p, std::size_t n);

struct InfoFrame {
    uint8_t type;  // 0 = FIS-B APDU, 14 = CRL, 15 = TIS-B/ADS-R service status
    const uint8_t* data;
    std::size_t len;
};

// Calls fn for each information frame in bytes [8, min(n, 432)) of a whole
// uplink payload. Stops at a zero-length frame or the end of the application
// data. Returns false (after delivering the frames before it) when a frame's
// length overruns the application data, or when n < 8.
bool forEachInfoFrame(const uint8_t* payload, std::size_t n, const std::function<void(const InfoFrame&)>& fn);

// The payload bytes of one dump978 / rawrec line. Accepts, around the hex:
// leading/trailing whitespace, an optional "<epoch_ms> " prefix (rawrec), an
// optional '+' or '-', and an optional ';' followed by ";"-separated metadata
// (rs=, rssi=, t=). The hex must be non-empty, of even length, and contain
// nothing else. Never throws.
bool hexToBytes(const std::string& line, std::vector<uint8_t>& out);

// True for an uplink ('+') line, with or without the rawrec epoch prefix.
// Downlinks ('-'), comments and bare hex are not uplinks.
bool isUplinkLine(const std::string& line);

}  // namespace fisb
}  // namespace skycued
