#pragma once

#include "track_table.hpp"

#include <skycue/types.hpp>

#include <chrono>

namespace skycued {

struct Thresholds {
    double min_move_m = 10, min_alt_change_ft = 50, min_gs_change_kt = 5, min_track_change_deg = 2;
    double min_interval_s = 1.0, max_interval_s = 10.0;
};

enum class SendReason { none, first, moved, altitude, speed, track, identity, max_interval };

// Whether the track's current state should go to the consumer, and why.
// Rules, first match wins:
//   1. no position in latest                                  -> none
//   2. never sent                                             -> first
//   3. callsign, squawk, emergency, on_ground or the ACAS RA
//      (appears, ends, or a different ARA) differs from what
//      was last sent (regardless of min_interval_s)           -> identity
//   4. less than min_interval_s since last_sent_at            -> none
//   5. great-circle move from the sent position >= min_move_m -> moved
//   6. altitude change >= min_alt_change_ft (baro; geom when
//      latest has no baro)                                    -> altitude
//   7. ground speed change >= min_gs_change_kt                -> speed
//   8. track change >= min_track_change_deg (wrap-aware)      -> track
//   9. max_interval_s or more since last_sent_at              -> max_interval
//  10.                                                        -> none
// For 6-8 a value present now but absent from the last send counts as a
// change; a value absent now never does.
SendReason shouldSend(const TrackState& state, std::chrono::steady_clock::time_point now, const Thresholds& t);

// Builds the wire Track from the state. vertical_rate_fpm is the baro rate if
// known, else the geometric rate.
skycue::Track toTrack(const TrackState& state, bool keyframe, bool alt_is_geom);

// Records that `sent` went to the consumer at `now` (sets last_sent and
// last_sent_at).
void markSent(TrackState& state, const skycue::Track& sent, std::chrono::steady_clock::time_point now);

} // namespace skycued
