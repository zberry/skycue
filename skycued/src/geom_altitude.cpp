#include "geom_altitude.hpp"

#include "track_table.hpp"

#include <algorithm>
#include <cmath>

namespace skycued {

namespace {

constexpr std::chrono::seconds kDiffPairWindow{2};
constexpr double kReportedMaxS = 60.0;
constexpr double kBaroMaxS = 10.0;
constexpr std::chrono::seconds kBaroNewerBy{2};
constexpr double kOwnDiffMaxS = 300.0;
constexpr double kDefaultSlope = 0.027;
constexpr int32_t kUncertaintyFresh = 75;
constexpr int32_t kUncertaintyAt60 = 125;
constexpr int32_t kUncertaintyAt300 = 325;
constexpr int32_t kFitUncertaintyFloor = 100;
constexpr int32_t kFitUncertaintyStep = 25;

double secondsBetween(Clock::time_point later, Clock::time_point earlier)
{
    return std::chrono::duration<double>(later - earlier).count();
}

int32_t ownDiffUncertainty(double age_s)
{
    if (age_s < kReportedMaxS)
        return kUncertaintyFresh;
    const double f = (age_s - kReportedMaxS) / (kOwnDiffMaxS - kReportedMaxS);
    return static_cast<int32_t>(std::lround(kUncertaintyAt60 + f * (kUncertaintyAt300 - kUncertaintyAt60)));
}

} // namespace

std::optional<int32_t> learnGeomDiff(GeomHistory& history, const TrackObservation& obs)
{
    const auto geom_t = obs.alt_geom_time.value_or(obs.received_time);
    const auto baro_t = obs.alt_baro_time.value_or(obs.received_time);
    int32_t diff = 0;
    if (obs.geom_minus_baro_ft) {
        diff = *obs.geom_minus_baro_ft;
    } else if (obs.alt_geom_ft && obs.alt_baro_ft) {
        const auto apart = geom_t > baro_t ? geom_t - baro_t : baro_t - geom_t;
        if (apart > kDiffPairWindow)
            return std::nullopt;
        diff = *obs.alt_geom_ft - *obs.alt_baro_ft;
    } else {
        return std::nullopt;
    }
    if (history.diff_ft && geom_t < history.diff_time)
        return std::nullopt;
    // A baro measured within 2 s of the difference pairs with it; only then is it
    // remembered with the difference and is the difference a sample for the fit.
    const bool paired = obs.alt_baro_ft && (geom_t > baro_t ? geom_t - baro_t : baro_t - geom_t) <= kDiffPairWindow;
    history.diff_ft = diff;
    history.baro_at_diff_ft = paired ? obs.alt_baro_ft : std::nullopt;
    history.diff_time = geom_t;
    if (!paired)
        return std::nullopt;
    return diff;
}

GeomResult resolveGeomAltitude(const TrackState& track, Clock::time_point now,
                               const std::optional<FitResult>& fit)
{
    GeomResult r;
    const TrackObservation& l = track.latest;
    r.geom_minus_baro_ft = track.geom.diff_ft;

    if (l.alt_geom_ft) {
        const double age = secondsBetween(now, track.alt_geom_at);
        const bool newer_baro = l.alt_baro_ft && track.alt_baro_at - track.alt_geom_at > kBaroNewerBy;
        if (age <= kReportedMaxS && !newer_baro) {
            r.source = GeomSource::reported;
            r.alt_geom_ft = l.alt_geom_ft;
            r.age_s = age;
            return r;
        }
    }

    if (!l.alt_baro_ft || secondsBetween(now, track.alt_baro_at) > kBaroMaxS)
        return r;
    if (l.on_ground && *l.on_ground)
        return r;
    const int32_t baro = *l.alt_baro_ft;

    if (track.geom.diff_ft) {
        const double age = secondsBetween(now, track.geom.diff_time);
        if (age <= kOwnDiffMaxS) {
            const double b = fit ? fit->b : kDefaultSlope;
            const double drift = track.geom.baro_at_diff_ft ? b * (baro - *track.geom.baro_at_diff_ft) : 0.0;
            r.source = GeomSource::estimated;
            r.alt_geom_ft = baro + *track.geom.diff_ft + static_cast<int32_t>(std::lround(drift));
            r.uncertainty_ft = ownDiffUncertainty(age);
            r.age_s = age;
            return r;
        }
    }

    if (fit) {
        const int32_t steps = static_cast<int32_t>(std::ceil(fit->p90_ft / kFitUncertaintyStep));
        r.source = GeomSource::estimated;
        r.alt_geom_ft = static_cast<int32_t>(std::lround(baro + fit->predict(baro)));
        r.uncertainty_ft = std::max(kFitUncertaintyFloor, steps * kFitUncertaintyStep);
        r.age_s = secondsBetween(now, fit->newest);
    }
    return r;
}

} // namespace skycued
