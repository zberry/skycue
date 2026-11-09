#include "track_table.hpp"

#include <utility>

namespace skycued {

namespace {

template <class T> void mergeField(std::optional<T>& into, const std::optional<T>& from)
{
    if (from)
        into = from;
}

constexpr std::chrono::seconds kRebroadcastHold{10};

bool hasPosition(const TrackObservation& o) { return o.lat.has_value() && o.lon.has_value(); }

} // namespace

TrackState& TrackTable::apply(const TrackObservation& obs)
{
    last_suppressed_ = false;
    auto it = tracks_.find(obs.id);
    const bool existing = it != tracks_.end();
    if (!existing) {
        TrackState fresh;
        fresh.id = obs.id;
        fresh.latest.id = obs.id;
        fresh.first_seen = obs.received_time;
        it = tracks_.emplace(obs.id, std::move(fresh)).first;
    }
    TrackState& s = it->second;
    TrackObservation& l = s.latest;

    if (!(obs.rebroadcast && existing)) {
        s.source = obs.source;
        l.source = obs.source;
    }
    l.received_time = obs.received_time;
    // An observation whose input does not say whether the target is on the
    // ground (on_ground unset) keeps the last known value.
    if (obs.on_ground)
        l.on_ground = obs.on_ground;
    l.raw_line = obs.raw_line;
    l.input_id = obs.input_id;
    s.last_input = obs.input_id;

    if (hasPosition(obs)) {
        const auto when = obs.position_time.value_or(obs.received_time);
        const bool direct_fresh =
            s.last_direct_position_time != std::chrono::system_clock::time_point{} &&
            when - s.last_direct_position_time < kRebroadcastHold;
        if (obs.rebroadcast && direct_fresh) {
            last_suppressed_ = true;
        } else {
            l.lat = obs.lat;
            l.lon = obs.lon;
            l.position_time = when;
            s.last_position_time = when;
            if (!obs.rebroadcast)
                s.last_direct_position_time = when;
        }
    }

    mergeField(l.alt_baro_ft, obs.alt_baro_ft);
    mergeField(l.alt_geom_ft, obs.alt_geom_ft);
    if (obs.alt_baro_ft)
        s.alt_baro_at = obs.alt_baro_time.value_or(obs.received_time);
    if (obs.alt_geom_ft)
        s.alt_geom_at = obs.alt_geom_time.value_or(obs.received_time);
    mergeField(l.gs_kt, obs.gs_kt);
    mergeField(l.track_deg, obs.track_deg);
    mergeField(l.baro_rate_fpm, obs.baro_rate_fpm);
    mergeField(l.geom_rate_fpm, obs.geom_rate_fpm);
    mergeField(l.callsign, obs.callsign);
    mergeField(l.squawk, obs.squawk);
    mergeField(l.registration, obs.registration);
    mergeField(l.emergency, obs.emergency);
    mergeField(l.category, obs.category);
    mergeField(l.nic, obs.nic);
    mergeField(l.nac_p, obs.nac_p);
    mergeField(l.rssi, obs.rssi);
    mergeField(l.heading_mag_deg, obs.heading_mag_deg);
    mergeField(l.heading_true_deg, obs.heading_true_deg);
    mergeField(l.ias_kt, obs.ias_kt);
    mergeField(l.tas_kt, obs.tas_kt);
    mergeField(l.mach, obs.mach);
    mergeField(l.roll_deg, obs.roll_deg);
    mergeField(l.track_rate_dps, obs.track_rate_dps);
    mergeField(l.sel_alt_ft, obs.sel_alt_ft);
    mergeField(l.sel_alt_fms_ft, obs.sel_alt_fms_ft);
    mergeField(l.qnh_mb, obs.qnh_mb);
    mergeField(l.sel_heading_deg, obs.sel_heading_deg);
    mergeField(l.nav_modes, obs.nav_modes);
    mergeField(l.acas_sl, obs.acas_sl);
    mergeField(l.acas_ri, obs.acas_ri);
    mergeField(l.acas_ra, obs.acas_ra);  // replaced whole
    // An advisory is shown for 30 s after it was received (or after a RAT set it inactive), whether or
    // not further messages arrive.
    if (l.acas_ra && obs.received_time - l.acas_ra->at > kAcasRaLife)
        l.acas_ra.reset();

    s.last_seen = obs.received_time;
    return s;
}

std::vector<skycue::TrackId> TrackTable::expire(std::chrono::system_clock::time_point now,
                                                std::chrono::seconds timeout)
{
    std::vector<skycue::TrackId> gone;
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        const TrackState& s = it->second;
        const auto ref = hasPosition(s.latest) ? s.last_position_time : s.last_seen;
        if (now - ref > timeout) {
            gone.push_back(it->first);
            it = tracks_.erase(it);
        } else {
            ++it;
        }
    }
    return gone;
}

TrackState* TrackTable::find(skycue::TrackId id)
{
    auto it = tracks_.find(id);
    return it == tracks_.end() ? nullptr : &it->second;
}

} // namespace skycued
