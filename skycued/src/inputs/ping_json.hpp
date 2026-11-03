#pragma once

// pingStation 3 JSON output (spec §5.1): one UDP datagram -> observations,
// device status and the raw frames for the raw capture. Pure; never throws.

#include "inputs/input_types.hpp"
#include "json_lite.hpp"
#include "observation.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skycued {

// Longer datagrams are rejected without parsing.
constexpr std::size_t kMaxDatagramBytes = 65536;

struct PingContext {
    std::chrono::system_clock::time_point arrival;  // daemon clock at datagram arrival
    TimeMode time_mode = TimeMode::utc;
};

// A `detail.rawData` frame: 7 or 14 bytes are 1090 (adsb_1090), 18 or 34 UAT
// (uat_978). `hex` is lowercase. `time` is the GPS counter time
// (timeOfReceptionS/NS) when utcSync is 1 and it passes the clock check, else
// arrival.
struct RawFrame {
    enum class Kind { adsb_1090, uat_978 } kind;
    std::string hex;
    std::chrono::system_clock::time_point time;
};

struct PingResult {
    std::vector<TrackObservation> observations;
    std::optional<DeviceStatus> status;
    std::vector<RawFrame> raw_frames;
    // rejected: a datagram that is oversize, not JSON or of no known shape,
    // each aircraft that does not map (no valid icaoAddress), a status that
    // does not parse. clock_rejects: timeStamps outside the clock window.
    // raw_unknown_len: rawData that is not hex of a known frame length.
    uint32_t rejected = 0, clock_rejects = 0, raw_unknown_len = 0;
    bool bad_squawk = false;  // a squawk was not a 4-digit octal display code (dropped)
    std::optional<double> clock_offset_s;  // of the last timeStamp checked
};

// Accepts a bare aircraft object, {"aircraft":[...]} and {"status":{...}}
// (aircraft and status may also come together). Each aircraft's timeStamp is
// checked against arrival when utcSync is 1 or absent (clock_offset_s,
// clock_rejects) whether or not the aircraft has a position; position_time is
// set only with a position. A raw frame's GPS counter time is clamped the same
// way but not counted. raw_line is the datagram text on the first observation
// of the datagram and empty on the others.
PingResult parsePingDatagram(const std::string& text, const PingContext& ctx);

// The object inside "status". nullopt when it holds none of the status fields.
// `version` is "major.minor.build" only when all three are integers, else
// empty; `device_time` is the parsed timeStamp, not clamped.
std::optional<DeviceStatus> parsePingStatus(const JsonObject& status_object,
                                            std::chrono::system_clock::time_point arrival);

// The {"status":{...}} body that GET /api/v1/status serves.
std::optional<DeviceStatus> parsePingStatusDocument(const std::string& text,
                                                    std::chrono::system_clock::time_point arrival);

}  // namespace skycued
