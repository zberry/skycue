#pragma once

// The web UI's JSON API (docs/web-api.md): the payloads, the /api/* routes
// and the static-file path rules. No sockets here: WebServer does the I/O.
//
// JSON: numbers and booleans are never quoted; a value that is not known
// (an unset optional, a NaN range or elevation) is left out rather than
// written as null; times are ISO-8601 UTC with milliseconds.

#include "engine_events.hpp"

#include <skycue/types.hpp>

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
std::string tilesAvailableJson(const std::string& www_dir);
std::string errorJson(const std::string& message);  // {"error":"..."}

// One item of a {"type":"events"} message: update, drop or stats (the
// filters/lists items are built from the current state: filtersJson,
// listsJson).
std::string eventJson(const EngineEvent& event);

struct Response {
    unsigned status = 200;
    std::string body;
    std::string content_type = "application/json";
    std::string allow;  // the Allow header of a 405
};

// True for "/api" and every path under "/api/".
bool isApiPath(const std::string& path);

// Answers one /api/* request (`path` without the query). Changes go through
// RuntimeConfig::applyUi with origin "ui". Never throws.
Response handleApi(const std::string& method, const std::string& path, const std::string& body, Engine& engine,
                   RuntimeConfig& runtime, const std::string& www_dir);

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
