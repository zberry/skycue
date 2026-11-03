#pragma once

// /api/inputs (spec §6.1, docs/web-api.md "Inputs"): the input definitions
// with their running state, and their create/replace/remove/enable/disable
// routes over the Engine's InputManager.

#include "inputs/input_manager.hpp"
#include "web_api.hpp"

#include <chrono>
#include <string>

namespace skycued {

class Engine;

namespace webapi {

// inputDefJson's keys, "builtin" and "state":{"connected", "receiving",
// "peers", "last_data_age_s"?, "clock_offset_s"?, "status_fail_streak",
// "last_error", every InputCounters field by its name, "device_status"?}.
// `now` is what last_data_age_s is measured from.
std::string inputJson(const InputEntry& entry, std::chrono::system_clock::time_point now);
// {"type"?, "inputs":[inputJson...]} in InputManager::list() order, at the
// wall clock now.
std::string inputsJson(const InputManager& inputs, const char* type = nullptr);

// "/api/inputs" and every path under "/api/inputs/".
bool isInputsPath(const std::string& path);
// The /api/inputs routes. Writes go through rejectUnsafe first. Errors:
// InputDefError and a body that is not a JSON object 400 (the message
// verbatim), InputForbidden 403, InputNotFound 404, InputConflict 409, any
// other std::runtime_error 500. Never throws.
ApiResponse handleInputsApi(const ApiRequest& request, Engine& engine);

}  // namespace webapi
}  // namespace skycued
