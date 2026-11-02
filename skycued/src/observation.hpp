#pragma once

#include <skycue/types.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
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

// Where the Engine gets its observations: readsb's JSON port or a replay.
// Threading: lives on the Engine's io_context; callbacks run on its thread.
// Register callbacks before start(). After stop() no callback is invoked.
class ObservationSource {
public:
    virtual ~ObservationSource() = default;
    virtual void onObservation(std::function<void(const TrackObservation&)> handler) = 0;
    // true when the source is (re)connected, false when it is lost (a
    // finished replay counts as lost).
    virtual void onConnection(std::function<void(bool)> handler) = 0;
    // UAT uplinks the source carries (a replay's "uat_uplink" records), with
    // their receive time and payload hex. Most sources have none.
    virtual void onUplink(std::function<void(std::chrono::system_clock::time_point, const std::string& hex)>) {}
    virtual void start() = 0;
    virtual void stop() = 0;
};

}  // namespace skycued
