#pragma once

// Small text conversions shared by the recorder and the replay source.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace skycued {

// "2026-11-01T12:34:56.789Z": UTC, rounded to the nearest millisecond.
std::string formatIso8601Ms(std::chrono::system_clock::time_point when);

// Inverse of formatIso8601Ms. Accepts exactly "YYYY-MM-DDTHH:MM:SS[.fff]Z"
// with 1 to 3 fraction digits (or none), from 1677-09-21T00:12:43.146Z to
// 2262-04-11T23:47:16.854Z (system_clock's int64 nanosecond range); anything
// else -> nullopt.
// Locale-independent; never throws.
std::optional<std::chrono::system_clock::time_point> parseIso8601Ms(const std::string& text);

// "20261101-123456": UTC, truncated to the second (for file names).
std::string formatCompactUtc(std::chrono::system_clock::time_point when);

// 0xa1b2c3 -> "a1b2c3" (6 lowercase hex digits; only the low 24 bits).
std::string hexAddress(uint32_t address);

// "a1b2c3" or "A1B2C3" -> 0xa1b2c3. Exactly 6 hex digits, else nullopt.
std::optional<uint32_t> parseHexAddress(const std::string& text);

}  // namespace skycued
