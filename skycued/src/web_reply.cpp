#include "web_reply.hpp"

#include "config.hpp"
#include "inputs/input_def.hpp"
#include "inputs/input_manager.hpp"
#include "json_lite.hpp"
#include "runtime_config.hpp"

#include <stdexcept>

namespace skycued {
namespace webapi {

std::optional<JsonObject> parseBodyObject(const std::string& body, bool strict)
{
    if (strict) {
        const auto first = body.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || body[first] != '{')
            return std::nullopt;
    }
    return JsonObject::parse(body);
}

ApiResponse reply(unsigned status, std::string body)
{
    ApiResponse r;
    r.status = status;
    r.body = std::move(body);
    return r;
}

ApiResponse fail(unsigned status, const std::string& message) { return reply(status, errorJson(message)); }

ApiResponse notAllowed(const char* allow)
{
    ApiResponse r = fail(405, "method not allowed");
    r.allow = allow;
    return r;
}

ApiResponse pinnedResponse(const PinnedKeyError& e)
{
    return reply(409, JsonWriter().add("error", std::string(e.what())).add("key", e.key).add("source", e.source).str());
}

ApiResponse failureResponse(const std::exception& e, ErrorFamily family)
{
    if (family == ErrorFamily::config) {
        if (const auto* p = dynamic_cast<const PinnedKeyError*>(&e))
            return pinnedResponse(*p);
        if (dynamic_cast<const std::invalid_argument*>(&e) || dynamic_cast<const ConfigError*>(&e))
            return fail(400, e.what());
        return fail(500, e.what());
    }
    if (dynamic_cast<const InputDefError*>(&e))
        return fail(400, e.what());
    if (dynamic_cast<const InputForbidden*>(&e))
        return fail(403, e.what());
    if (dynamic_cast<const InputNotFound*>(&e))
        return fail(404, e.what());
    if (dynamic_cast<const InputConflict*>(&e))
        return fail(409, e.what());
    return fail(500, e.what());
}

}  // namespace webapi
}  // namespace skycued
