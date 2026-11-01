#include "track_table.hpp"

#include <utility>

namespace skycued {

namespace {

template <class T> void mergeField(std::optional<T>& into, const std::optional<T>& from)
{
    if (from)
        into = from;
}

bool hasPosition(const TrackObservation& o) { return o.lat.has_value() && o.lon.has_value(); }

} // namespace

TrackState& TrackTable::apply(const TrackObservation& obs)
{
    auto it = tracks_.find(obs.id);
    if (it == tracks_.end()) {
        TrackState fresh;
        fresh.id = obs.id;
        fresh.latest.id = obs.id;
        it = tracks_.emplace(obs.id, std::move(fresh)).first;
    }
    TrackState& s = it->second;
    TrackObservation& l = s.latest;

    s.source = obs.source;
    l.source = obs.source;
    l.received_time = obs.received_time;
    l.on_ground = obs.on_ground;
    l.raw_line = obs.raw_line;

    if (hasPosition(obs)) {
        l.lat = obs.lat;
        l.lon = obs.lon;
        l.position_time = obs.position_time.value_or(obs.received_time);
        s.last_position_time = *l.position_time;
    }

    mergeField(l.alt_baro_ft, obs.alt_baro_ft);
    mergeField(l.alt_geom_ft, obs.alt_geom_ft);
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
