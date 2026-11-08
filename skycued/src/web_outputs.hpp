#pragma once

// /api/outputs (docs/web-api.md "Outputs"): the output definitions with their
// running state, and their create/replace/remove/enable/disable routes, the
// mirror of /api/inputs. The definitions live in the InputManager (it owns the
// inputs file); the state comes from the Hub, which lists the cue stream's
// built-in outputs first.

#include "hub/hub.hpp"
#include "web_api.hpp"

#include <string>

namespace skycued {

class Engine;

namespace webapi {

// outputDefJson's keys, "builtin" and "state":{"up", "peers", "bytes",
// "messages", "dropped", "last_error"}.
std::string outputJson(const OutputState& state);
// [outputJson...] in Hub::list() order.
std::string outputsArrayJson(const Hub& hub);
// {"type"?, "outputs":[outputJson...]}; `type`, when given, is written first.
std::string outputsJson(const Hub& hub, const char* type = nullptr);

// "/api/outputs" and every path under "/api/outputs/".
bool isOutputsPath(const std::string& path);
// The /api/outputs routes. Writes go through rejectUnsafe first. Errors:
// InputDefError and a body that is not a JSON object 400 (the message
// verbatim), InputForbidden 403, InputNotFound 404, InputConflict 409, any
// other std::runtime_error 500. Never throws.
ApiResponse handleOutputsApi(const ApiRequest& request, Engine& engine);

}  // namespace webapi
}  // namespace skycued
