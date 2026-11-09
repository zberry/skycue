#pragma once

// JSON shapes of the Comm-B, target-state and ACAS fields of an observation, shared by the
// web API and the received recording (docs/web-api.md, docs/recording-format.md).

#include "json_lite.hpp"
#include "observation.hpp"

#include <optional>
#include <string>
#include <vector>

namespace skycued {

// The names of the bits of a nav_modes mask, lowest bit first.
std::vector<std::string> navModeNames(uint8_t mask);
// "[\"autopilot\",...]"
std::string navModesJson(uint8_t mask);
// The mask of the array `key`; nullopt when it is not an array of known names.
std::optional<uint8_t> parseNavModes(const JsonObject& j, const char* key);

// {"active":..,"ara":..,"rac":..,"rat":..,"mte":..,"text":..,"threat":{"type":"address"|"position",...}};
// threat is omitted when tti is 0. `at` is not part of it.
std::string acasRaJson(const AcasRaState& ra);
// The inverse; `at` is left at its default. nullopt for a malformed object.
std::optional<AcasRaState> parseAcasRa(const JsonObject& o);

}  // namespace skycued
