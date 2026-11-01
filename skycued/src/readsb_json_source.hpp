#pragma once

#include "observation.hpp"

#include <optional>
#include <string>

namespace skycued {

// Parses one line from readsb's JSON output port (--net-json-port): an
// aircraft.json aircraft object with a leading "now" (epoch seconds).
// Requires a valid "hex" (exactly 6 hex digits, optionally "~"-prefixed) and a
// numeric "now"; unknown keys, arrays and nested objects are ignored.
// nullopt -> the caller logs and skips the line. Never throws.
std::optional<TrackObservation> parseReadsbLine(const std::string& line);

}  // namespace skycued
