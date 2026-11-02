#pragma once

// The FIS-B part of the web API (docs/web-api.md "FIS-B"): the /api/fisb/*
// routes, the fisb_* WebSocket items and the status object that the
// snapshot carries. Record bodies come from fisb_json (the names the fisb
// recording uses); this file defines only the envelopes, the GeoJSON-like
// feature collection and the palettes/legends.

#include "engine_events.hpp"
#include "fisb/fisb_store.hpp"

#include <string>

namespace skycued {

class Engine;

namespace webapi {

struct ApiRequest;
struct ApiResponse;

// The FIS-B status object (GET /api/fisb/status, the snapshot's "fisb"):
// counters, stations, mosaic heads and record counts, never blocks. `snap`
// gives the data clock and the counters, `fisb` the products.
std::string fisbStatusJson(const Snapshot& snap, const fisb::FisbSnapshot& fisb);

// One fisb_* item of an {"type":"events"} message; "" for another kind or
// an event without its notice.
std::string fisbEventJson(const EngineEvent& event);

// The GeoJSON Features of one TWGO record, one per shape: "[...]".
std::string twgoFeaturesJson(const fisb::TwgoRecord& record);

// True for every path under "/api/fisb/".
bool isFisbPath(const std::string& path);
// Answers one /api/fisb/* request (all GET). May throw; handleApi turns
// that into a 500.
ApiResponse handleFisbApi(const ApiRequest& request, const Engine& engine);

}  // namespace webapi
}  // namespace skycued
