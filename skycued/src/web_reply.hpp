#pragma once

// The reply helpers and the exception-to-status ladders the /api/* route files share.
// Two error families stay separate on purpose: std::invalid_argument is a server error in
// the resource routes (inputs, outputs) and a bad request in the config routes.

#include "json_lite.hpp"
#include "web_api.hpp"

#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace skycued {

struct PinnedKeyError;

namespace webapi {

// A JSON reply; content_type stays "application/json".
ApiResponse reply(unsigned status, std::string body);
// {"error":"<message>"} with the given status.
ApiResponse fail(unsigned status, const std::string& message);
// 405 {"error":"method not allowed"} with the Allow header set to `allow`.
ApiResponse notAllowed(const char* allow);
// 409 {"error":<what>,"key":<key>,"source":<source>}: the key is pinned by --set or the environment.
ApiResponse pinnedResponse(const PinnedKeyError& e);

// The body as a JSON object, or nullopt when it is not one. The first non-whitespace
// character must be '{' (the parser alone reads "[]" as an empty object), and
// JsonObject::parse must accept it. Every route that reads an object body answers
// nullopt with 400 kNotObjectMessage (notObjectResponse()).
std::optional<JsonObject> parseBodyObject(const std::string& body);
inline constexpr const char* kNotObjectMessage = "the body must be a JSON object";
// 400 {"error":"the body must be a JSON object"}.
ApiResponse notObjectResponse();

enum class ErrorFamily { resource, config };

// The status for an exception; the message is e.what().
//   resource: InputDefError 400, InputForbidden 403, InputNotFound 404, InputConflict 409, other 500.
//   config:   PinnedKeyError (pinnedResponse), std::invalid_argument 400, ConfigError 400, other 500.
ApiResponse failureResponse(const std::exception& e, ErrorFamily family);

// Runs `f`, turning an exception into the resource-family reply.
template <class F> ApiResponse runGuarded(F&& f)
{
    try {
        return f();
    } catch (const std::exception& e) {
        return failureResponse(e, ErrorFamily::resource);
    }
}

}  // namespace webapi
}  // namespace skycued
