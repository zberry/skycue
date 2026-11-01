#pragma once

#include <skycue/types.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace skycued {

// One report about one aircraft from a source (a readsb or dump978 line),
// before filtering and rate limiting turn it into a skycue::Track.
struct TrackObservation {
    skycue::TrackId id;
    skycue::Source source{skycue::Source::other};
    std::chrono::system_clock::time_point received_time{};               // readsb "now"
    std::optional<std::chrono::system_clock::time_point> position_time;  // now - seen_pos; set with lat/lon
    std::optional<double> lat, lon;                                      // both or neither
    bool on_ground = false;
    std::optional<int32_t> alt_baro_ft, alt_geom_ft;
    std::optional<float> gs_kt, track_deg, baro_rate_fpm, geom_rate_fpm;
    std::optional<std::string> callsign, squawk, registration, emergency, category;
    std::optional<uint8_t> nic, nac_p;
    std::optional<double> rssi;
    std::string raw_line;  // kept for the received recording
};

}  // namespace skycued
