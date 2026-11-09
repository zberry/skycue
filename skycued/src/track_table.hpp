#pragma once

#include "geom_altitude.hpp"
#include "observation.hpp"

#include <skycue/types.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace skycued {

// Everything skycued knows about one track.
struct TrackState {
    skycue::TrackId id;
    skycue::Source source{skycue::Source::other};
    // Merged observation: each optional field keeps the last known value when a
    // newer observation lacks it; lat/lon/position_time move together.
    // received_time, source, raw_line and input_id always come from the
    // newest; on_ground from the newest that sets it.
    TrackObservation latest;
    GeomHistory geom;                                           // last reported GNSS-minus-baro (learnGeomDiff)
    // When the merged alt_baro_ft / alt_geom_ft were measured (obs.alt_*_time, else received_time).
    std::chrono::system_clock::time_point alt_baro_at{}, alt_geom_at{};
    std::string last_input;                                     // the newest observation's input_id
    std::chrono::system_clock::time_point first_seen{};         // received_time of the first observation
    std::chrono::system_clock::time_point last_seen{};          // newest received_time
    std::chrono::system_clock::time_point last_position_time{}; // newest position time (if ever positioned)
    std::chrono::system_clock::time_point last_direct_position_time{}; // newest position of a non-rebroadcast observation
    std::optional<skycue::Track> last_sent;                     // what the consumer last received
    std::chrono::steady_clock::time_point last_sent_at{};
    bool currently_forwarded = false;
};

class TrackTable {
public:
    // Creates the track or merges the observation into it; returns its state.
    // The reference stays valid until the track is expired.
    // A rebroadcast observation's position is ignored while the track has a
    // direct position younger than 10 s; its other fields still merge and it
    // never changes the track's source.
    TrackState& apply(const TrackObservation& obs);
    // Whether the last apply() ignored a position for that reason.
    bool lastApplySuppressedPosition() const { return last_suppressed_; }

    // Removes and returns the tracks whose last_position_time (or last_seen if
    // they never had a position) is more than `timeout` before `now`.
    std::vector<skycue::TrackId> expire(std::chrono::system_clock::time_point now, std::chrono::seconds timeout);

    TrackState* find(skycue::TrackId id);
    size_t size() const { return tracks_.size(); }

    template <class F> void forEach(F f)
    {
        for (auto& kv : tracks_)
            f(kv.second);
    }
    template <class F> void forEach(F f) const
    {
        for (const auto& kv : tracks_)
            f(kv.second);
    }

private:
    std::unordered_map<skycue::TrackId, TrackState> tracks_;
    bool last_suppressed_ = false;
};

} // namespace skycued
