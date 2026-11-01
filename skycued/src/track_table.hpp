#pragma once

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
    // on_ground, received_time, source and raw_line always come from the newest.
    TrackObservation latest;
    std::chrono::system_clock::time_point last_seen{};          // newest received_time
    std::chrono::system_clock::time_point last_position_time{}; // newest position time (if ever positioned)
    std::optional<skycue::Track> last_sent;                     // what the consumer last received
    std::optional<std::string> last_sent_emergency;             // latest.emergency at that send (Track has no field)
    std::chrono::steady_clock::time_point last_sent_at{};
    bool currently_forwarded = false;
};

class TrackTable {
public:
    // Creates the track or merges the observation into it; returns its state.
    // The reference stays valid until the track is expired.
    TrackState& apply(const TrackObservation& obs);

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

private:
    std::unordered_map<skycue::TrackId, TrackState> tracks_;
};

} // namespace skycued
