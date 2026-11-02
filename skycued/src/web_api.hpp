#pragma once

// The web UI's JSON API (docs/web-api.md): the payloads, the /api/* routes
// and the static-file path rules. No sockets here: WebServer does the I/O.
//
// JSON: numbers and booleans are never quoted; a value that is not known
// (an unset optional, a NaN range or elevation) is left out rather than
// written as null; times are ISO-8601 UTC with milliseconds.

#include "engine_events.hpp"

#include <skycue/types.hpp>

#include <map>
#include <optional>
#include <string>

namespace skycued {

class Engine;
class RuntimeConfig;

namespace webapi {

// Each of these is one JSON object. `type`, when given, is written first as
// "type":"<type>" (for the WebSocket messages).
std::string stateJson(const Engine& engine, const RuntimeConfig& runtime, const char* type = nullptr);
std::string statsJson(const EngineStats& stats, const char* type = nullptr);
std::string filtersJson(const RuntimeConfig& runtime, const char* type = nullptr);
std::string listsJson(const Engine& engine, const RuntimeConfig& runtime, const char* type = nullptr);
std::string trackJson(const TrackSnapshot& track, const char* type = nullptr);
std::string dropJson(skycue::TrackId id, skycue::DropReason reason);  // {"type":"drop",...}
std::string expiredJson(skycue::TrackId id);                          // {"type":"expired",...}
std::string tilesAvailableJson(const std::string& www_dir);
std::string errorJson(const std::string& message);  // {"error":"..."}

// One item of a {"type":"events"} message: observed, update, drop, expired or stats (the
// filters/lists items are built from the current state: filtersJson,
// listsJson).
std::string eventJson(const EngineEvent& event);

struct ApiResponse {
    unsigned status = 200;
    std::string body;
    std::string content_type = "application/json";
    std::string allow;  // the Allow header of a 405
};

// One HTTP request as the API sees it.
struct ApiRequest {
    std::string method;                         // "GET", "PUT", ...
    std::string path;                           // the target without the query
    std::string query;                          // after '?' (without it), "" when none
    std::map<std::string, std::string> headers;  // names lower-cased
    std::string body;
};

// What the routes work on.
struct ApiContext {
    Engine& engine;
    RuntimeConfig& runtime;
    std::string www_dir;
};

// True for "/api" and every path under "/api/".
bool isApiPath(const std::string& path);

// The query string "a=1&b=x%20y" as {a: "1", b: "x y"}: '+' is a space,
// %XX escapes are decoded (a malformed one is kept as written); a key
// without '=' has the value ""; of a repeated key the last wins.
std::map<std::string, std::string> queryParams(const std::string& query);

// Answers one /api/* request. A PUT or POST must have a Content-Type of
// application/json (else 415) and, when it has an Origin header, one whose
// host:port is the Host header's (else 403): a page from another site
// cannot change the filters through a browser. Changes go through
// RuntimeConfig::applyUi with origin "ui". Never throws.
ApiResponse handleApi(const ApiRequest& request, ApiContext& context);

// The file path under the www directory that a request target names: the
// query is dropped, %XX escapes are decoded, "/" becomes "/index.html".
// nullopt (a 400) unless the result starts with '/' and every segment is
// non-empty and neither "." nor "..", with no NUL or backslash anywhere and
// no malformed escape.
std::optional<std::string> staticPath(const std::string& target);
// Content-Type by extension (application/octet-stream when unknown).
const char* contentType(const std::string& path);
// "max-age=86400" under /tiles/ and /vendor/, else "no-cache".
const char* cacheControl(const std::string& path);

}  // namespace webapi
}  // namespace skycued
