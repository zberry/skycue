#include "change_detector.hpp"

#include "geo.hpp"

#include <cmath>
#include <optional>

namespace skycued {

namespace {

// |now - prev| >= threshold; a value that newly appeared counts as a change.
template <class T> bool changed(const std::optional<T>& now, const std::optional<T>& prev, double threshold)
{
    if (!now)
        return false;
    if (!prev)
        return true;
    return std::fabs(static_cast<double>(*now) - static_cast<double>(*prev)) >= threshold;
}

double angleDiffDeg(double a, double b)
{
    double d = std::fmod(std::fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

} // namespace

SendReason shouldSend(const TrackState& s, std::chrono::steady_clock::time_point now, const Thresholds& t)
{
    const TrackObservation& l = s.latest;
    if (!l.lat || !l.lon)
        return SendReason::none;
    if (!s.last_sent)
        return SendReason::first;
    const skycue::Track& p = *s.last_sent;

    if (l.callsign != p.callsign || l.squawk != p.squawk || l.emergency != s.last_sent_emergency ||
        l.on_ground != p.on_ground)
        return SendReason::identity;

    const double elapsed = std::chrono::duration<double>(now - s.last_sent_at).count();
    if (elapsed < t.min_interval_s)
        return SendReason::none;

    const double moved_m = geo::greatCircleNm(p.lat_deg, p.lon_deg, *l.lat, *l.lon) * geo::kMetresPerNm;
    if (moved_m >= t.min_move_m)
        return SendReason::moved;

    const bool baro = l.alt_baro_ft.has_value();
    if (changed(baro ? l.alt_baro_ft : l.alt_geom_ft, baro ? p.alt_baro_ft : p.alt_geom_ft, t.min_alt_change_ft))
        return SendReason::altitude;

    if (changed(l.gs_kt, p.ground_speed_kt, t.min_gs_change_kt))
        return SendReason::speed;

    if (l.track_deg && (!p.track_deg || angleDiffDeg(*l.track_deg, *p.track_deg) >= t.min_track_change_deg))
        return SendReason::track;

    if (elapsed >= t.max_interval_s)
        return SendReason::max_interval;
    return SendReason::none;
}

skycue::Track toTrack(const TrackState& s, bool keyframe, bool alt_is_geom)
{
    const TrackObservation& l = s.latest;
    skycue::Track t;
    t.id = s.id;
    t.source = s.source;
    t.position_time = s.last_position_time;
    t.lat_deg = l.lat.value_or(0.0);
    t.lon_deg = l.lon.value_or(0.0);
    t.on_ground = l.on_ground;
    t.keyframe = keyframe;
    t.alt_is_geom = alt_is_geom;
    t.alt_baro_ft = l.alt_baro_ft;
    t.alt_geom_ft = l.alt_geom_ft;
    t.ground_speed_kt = l.gs_kt;
    t.track_deg = l.track_deg;
    t.vertical_rate_fpm = l.baro_rate_fpm ? l.baro_rate_fpm : l.geom_rate_fpm;
    t.callsign = l.callsign;
    t.squawk = l.squawk;
    t.registration = l.registration;
    t.nic = l.nic;
    t.nac_p = l.nac_p;
    return t;
}

void markSent(TrackState& s, const skycue::Track& sent, std::chrono::steady_clock::time_point now)
{
    s.last_sent = sent;
    s.last_sent_at = now;
    s.last_sent_emergency = s.latest.emergency;
}

} // namespace skycued
