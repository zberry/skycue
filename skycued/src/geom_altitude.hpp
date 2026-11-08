#pragma once

#include "altitude_fit.hpp"
#include "observation.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace skycued {

struct TrackState;

using Clock = std::chrono::system_clock;

enum class GeomSource { none, reported, estimated };

// The last GNSS-minus-baro difference a track reported (lives in TrackState).
struct GeomHistory {
    std::optional<int32_t> diff_ft;          // last reported GNSS-minus-baro
    std::optional<int32_t> baro_at_diff_ft;  // the baro altitude paired with it, when known
    Clock::time_point diff_time{};           // when that difference was measured
};

struct GeomResult {
    GeomSource source = GeomSource::none;
    std::optional<int32_t> alt_geom_ft, uncertainty_ft;  // uncertainty only when estimated
    // reported: age of the report; estimated: age of the basis (own difference, else fit newest)
    std::optional<double> age_s;
    std::optional<int32_t> geom_minus_baro_ft;  // last reported difference, if any
};

// Learns a difference from the observation: obs.geom_minus_baro_ft, else geom - baro when
// both measurement times (alt_*_time, absent = received_time) are within 2 s. Updates the
// history and returns the difference as a sample for the fit; nullopt when the observation
// has none (or it is older than the one held).
std::optional<int32_t> learnGeomDiff(GeomHistory& history, const TrackObservation& obs);

// A reported alt_geom_ft measured <= 60 s ago is `reported`. Otherwise, with a baro <= 10 s
// old and the track not on the ground: own difference <= 300 s old, else the fit, else none.
GeomResult resolveGeomAltitude(const TrackState& track, Clock::time_point now,
                               const std::optional<FitResult>& fit);

}  // namespace skycued
