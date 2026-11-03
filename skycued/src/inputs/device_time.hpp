#pragma once

// Device timestamps: parsing, the ±10 s clamp against the daemon's clock
// (spec §4.5), seconds-of-day dating and the pingStation GPS counter.
// Pure functions; never throw.

#include "inputs/input_types.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace skycued {

// A device time further than this from the arrival time is not used.
constexpr std::chrono::seconds kMaxDeviceSkew{10};
// GPS time minus UTC (leap seconds since 1980), subtracted in TimeMode::gps.
constexpr std::chrono::seconds kGpsUtcOffset{18};

// "YYYY-MM-DDTHH:MM:SS", then optionally `.` or `:` and 1 to 9 fraction
// digits, then "Z" (the ICD writes both "…:32.3725496Z" and
// "…:ss:ffffffffZ"). The date and time fields are range-checked (no leap
// second). Anything else -> nullopt. UTC; locale- and time-zone-independent.
std::optional<std::chrono::system_clock::time_point> parseDeviceTime(const std::string& text);

struct ClockCheck {
    std::chrono::system_clock::time_point used;  // the device time, or arrival
    std::optional<double> offset_s;  // device time (after the gps correction) minus arrival
    bool rejected = false;           // |offset_s| > kMaxDeviceSkew, so `used` is arrival
};

// With no device time: arrival, no offset, not rejected. Otherwise subtracts
// kGpsUtcOffset for TimeMode::gps, measures the offset against arrival and
// uses the device time only when |offset| <= kMaxDeviceSkew.
ClockCheck checkDeviceTime(std::optional<std::chrono::system_clock::time_point> device,
                           std::chrono::system_clock::time_point arrival, TimeMode mode);

// A seconds-of-day value (ASTERIX 071/073/075/077) dated with the UTC date of
// `now`: a time-of-day more than 12 h ahead of now's belongs to the previous
// day, more than 12 h behind to the next day.
std::chrono::system_clock::time_point timeOfDay(double tod_s, std::chrono::system_clock::time_point now);

// The pingStation's timeOfReceptionS/NS: seconds since the GPS epoch
// (1980-01-06, Unix 315964800) plus nanoseconds. No leap-second correction: in
// the ICD example the counter equals the UTC timeStamp.
std::chrono::system_clock::time_point gpsCounterTime(uint64_t s, uint64_t ns);

}  // namespace skycued
