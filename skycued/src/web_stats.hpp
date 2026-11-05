#pragma once

// The statistics part of the web API (docs/web-api.md "Statistics"): the
// /api/stats/* routes over the Engine's StatsStore (the
// stats view design, §4-§5).
// The exact path /api/stats (the engine's lifetime counters) is web_api's.
//
// Every route reads the store on the io thread, as the WebServer calls
// handleApi there; none copies the track table (no Engine::snapshot()).

#include <chrono>
#include <optional>
#include <string>

namespace skycued {

class Engine;
class StatsStore;

namespace webapi {

struct ApiRequest;
struct ApiResponse;

// What the routes keep between requests (one per WebServer).
struct StatsApiState {
    // The last accepted POST /api/stats/reset; another within kStatsResetGap is a 429.
    std::optional<std::chrono::steady_clock::time_point> last_reset;
};
constexpr std::chrono::seconds kStatsResetGap{5};

// True for every path under "/api/stats/" (not "/api/stats" itself).
bool isStatsPath(const std::string& path);

// The GET routes over `store` with the data clock `now`: summary, series,
// coverage, rssi, aircraft and aircraft/{hex}; 404 for another path, 405
// for another method. May throw; handleApi turns that into a 500.
ApiResponse statsReadRoute(const ApiRequest& request, const StatsStore& store,
                           std::chrono::system_clock::time_point now);

// Answers one /api/stats/* request: POST /api/stats/reset (rate-limited
// through `state` when not null; Engine::resetStats("ui <peer>")), the rest
// through statsReadRoute on engine.statsStore() and engine.dataClock().
ApiResponse handleStatsApi(const ApiRequest& request, Engine& engine, StatsApiState* state);

}  // namespace webapi
}  // namespace skycued
