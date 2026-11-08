#include "decode/uat_decoder.hpp"

#include <algorithm>
#include <cmath>

#include "coding/bits.hpp"

// Rules: decoder spec sections 5.4 (altitude), 6 (address qualifiers) and 7
// (observation production). Constants marked "our choice" are not cited values.

namespace skycued::decode {

namespace {

using std::chrono::seconds;
using skycue::AddressType;
using skycue::Source;

constexpr seconds kFieldFresh{60};     // our choice: observations carry fields <= 60 s old
constexpr double kRangeNm = 300.0;     // our choice: same reasonableness range as the 1090 decoder
constexpr std::size_t kRssiFrames = 8; // our choice: rssi is the mean power over the last 8 frames

void pushRssi(UatKeyState& ks, double dbfs) {
    ks.rssi_ring[ks.rssi_next] = dbfs;
    ks.rssi_next = (ks.rssi_next + 1) % kRssiFrames;
    if (ks.rssi_count < kRssiFrames) ++ks.rssi_count;
}

std::optional<double> rssiOf(const UatKeyState& ks) {
    if (ks.rssi_count == 0) return std::nullopt;
    double sum = 0.0;
    for (std::size_t i = 0; i < ks.rssi_count; ++i) sum += std::pow(10.0, ks.rssi_ring[i] / 10.0);
    const double mean = sum / static_cast<double>(ks.rssi_count);
    if (!(mean > 0.0) || !std::isfinite(mean)) return std::nullopt;
    return std::round(10.0 * std::log10(mean) * 10.0) / 10.0;
}

// Sets a timed field; flags a change when it is new, expired or replaced a different value.
template <typename T>
void setField(UatKeyState& ks, Timed<T>& f, const T& v, TimePoint now) {
    if (!f.fresh(now, kFieldFresh) || !(f.value == v)) ks.dirty = true;
    f.set(v, now);
}

// Identity fields live with the key (spec 14 item 5): changed when new or different, never expired.
template <typename T>
void setIdentity(UatKeyState& ks, Timed<T>& f, const T& v, TimePoint now) {
    if (!f.has || !(f.value == v)) ks.dirty = true;
    f.set(v, now);
}

}  // namespace

UatDecoder::UatDecoder(DecoderConfig cfg, std::string input_id) : cfg_(cfg), input_id_(std::move(input_id)) {}

void UatDecoder::onObservation(std::function<void(const skycued::TrackObservation&)> h) { handler_ = std::move(h); }
const DecodeCounters& UatDecoder::counters() const { return counters_; }
std::size_t UatDecoder::keys() const { return keys_.size(); }

UatKeyState& UatDecoder::obtain(const StateKey& k, Source src) {
    auto it = keys_.find(k);
    UatKeyState* ks = it == keys_.end() ? nullptr : &it->second;
    if (!ks) {
        if (cfg_.max_keys > 0 && keys_.size() >= cfg_.max_keys && !lru_.empty()) {
            keys_.erase(lru_.front());  // oldest first when full
            lru_.pop_front();
            ++counters_.evicted;
        }
        lru_.push_back(k);
        ks = &keys_[k];
        ks->key = k;
        ks->lru = std::prev(lru_.end());
    } else {
        lru_.splice(lru_.end(), lru_, ks->lru);
    }
    ks->last_seen = now_;
    ks->source = src;
    return *ks;
}

void UatDecoder::evictIdle() {
    while (!lru_.empty()) {
        const auto it = keys_.find(lru_.front());
        if (it != keys_.end() && now_ - it->second.last_seen <= cfg_.idle_evict) break;
        if (it != keys_.end()) keys_.erase(it);
        lru_.pop_front();
        ++counters_.evicted;
    }
}

void UatDecoder::feed(const uat::UatFrame& f, Clock::time_point arrival, std::optional<double> rssi_dbfs) {
    ++counters_.frames;
    const auto map = uat::mapQualifier(f.qualifier());
    if (!map || !f.lengthMatchesType()) {
        ++counters_.unknown_type;
        return;
    }
    // Max-so-far time: arrival times may step back, windows never do.
    if (!have_now_ || arrival > now_) {
        now_ = arrival;
        have_now_ = true;
    }
    evictIdle();

    StateKey key;
    key.address = f.address();
    key.type = map->type;
    key.rebroadcast = map->rebroadcast;
    UatKeyState& ks = obtain(key, map->source);
    ks.dirty = false;
    const auto level = rssi_dbfs ? rssi_dbfs : f.rssi_dbfs;
    if (level && std::isfinite(*level)) pushRssi(ks, *level);

    const uat::StateVector sv = uat::decodeStateVector(f);
    const bool trackfile = map->type == AddressType::tisb_trackfile;  // TIS-B trackfile targets carry no geometric altitude

    // Altitudes: the state vector's own, then the AUX SV's secondary one for the other type.
    std::optional<int32_t> baro, geom;
    if (sv.altitude_ft) (sv.alt_type == uat::AltType::baro ? baro : geom) = sv.altitude_ft;
    if (const auto aux = uat::decodeAuxSv(f)) (sv.alt_type == uat::AltType::baro ? geom : baro) = aux;
    if (trackfile) geom.reset();
    std::optional<int32_t> frame_diff;
    if (baro && geom) frame_diff = *geom - *baro;  // both from this one frame

    // Ground state: asserted only by a frame whose A/G state says so.
    std::optional<bool> frame_ground;
    if (sv.ag == uat::StateVector::Ag::ground) frame_ground = true;
    else if (sv.ag != uat::StateVector::Ag::reserved) frame_ground = false;
    if (frame_ground) setField(ks, ks.on_ground, *frame_ground, now_);
    const bool ground = ks.on_ground.fresh(now_, kFieldFresh).value_or(false);

    if (baro) {
        if (ground) ks.alt_baro.set(*baro, now_);  // stored, but a ground target does not report it
        else setField(ks, ks.alt_baro, *baro, now_);
    }
    if (geom) setField(ks, ks.alt_geom, *geom, now_);
    if (sv.gs_kt) setField(ks, ks.gs, *sv.gs_kt, now_);
    if (sv.track_deg) setField(ks, ks.track, *sv.track_deg, now_);
    if (sv.vrate_fpm) {
        const float r = static_cast<float>(*sv.vrate_fpm);
        if (sv.vrate_geometric.value_or(false)) ks.geom_rate.set(r, now_);
        else ks.baro_rate.set(r, now_);
    }
    setField(ks, ks.nic, sv.nic, now_);

    if (const auto ms = uat::decodeModeStatus(f)) {
        if (!ms->emitter_category.empty()) setIdentity(ks, ks.category, ms->emitter_category, now_);
        if (ms->callsign) setIdentity(ks, ks.callsign, *ms->callsign, now_);
        if (ms->squawk) setIdentity(ks, ks.squawk, *ms->squawk, now_);
        if (ms->emergency) setIdentity(ks, ks.emergency, *ms->emergency, now_);
        if (ms->nac_p) setField(ks, ks.nac_p, *ms->nac_p, now_);
    }

    // Position: a whole fix per frame, checked for reasonableness only (no CPR).
    std::optional<cpr::Position> accepted;
    bool dropped = false;
    if (sv.lat && sv.lon) {
        const cpr::Position p{*sv.lat, *sv.lon};
        const std::optional<cpr::Position> anchor = cfg_.reference ? cfg_.reference : ks.first_pos;
        if (!std::isfinite(p.lat) || !std::isfinite(p.lon) || std::fabs(p.lat) > 90.0 ||
            (anchor && cpr::distanceNm(p, *anchor) > kRangeNm)) {
            ++counters_.cpr_rejected;  // the position is dropped, no stored position is touched
            dropped = true;
        } else {
            if (!ks.first_pos) ks.first_pos = p;
            accepted = p;  // every accepted in-range fix is new, even a repeat: position_time must advance
            ks.last_pos = p;
        }
    }

    // Counters: one of unknown_type (above), no_position, unmapped, decoded per frame.
    if (!sv.lat || !sv.lon || dropped) ++counters_.no_position;
    else if (f.payload_type() >= 7) ++counters_.unmapped;  // target state / trajectory change content is not decoded
    else ++counters_.decoded;

    if (!accepted && !ks.dirty) return;
    if (!handler_) return;

    skycued::TrackObservation o;
    o.id.address = ks.key.address;
    o.id.type = ks.key.type;
    o.source = ks.source;
    o.received_time = arrival;
    if (accepted) {
        o.lat = accepted->lat;
        o.lon = accepted->lon;
        o.position_time = arrival;
    }
    o.on_ground = frame_ground;
    if (!ground) o.alt_baro_ft = ks.alt_baro.fresh(now_, kFieldFresh);  // ground suppresses it (spec 5.4)
    if (o.alt_baro_ft) o.alt_baro_time = ks.alt_baro.at;
    o.alt_geom_ft = ks.alt_geom.fresh(now_, kFieldFresh);
    if (o.alt_geom_ft) o.alt_geom_time = ks.alt_geom.at;
    if (frame_diff && o.alt_baro_ft && o.alt_geom_ft) {  // only a difference this frame measured
        o.geom_minus_baro_ft = frame_diff;
        o.geom_minus_baro_time = now_;
    }
    o.gs_kt = ks.gs.fresh(now_, kFieldFresh);
    o.track_deg = ks.track.fresh(now_, kFieldFresh);
    o.baro_rate_fpm = ks.baro_rate.fresh(now_, kFieldFresh);
    o.geom_rate_fpm = ks.geom_rate.fresh(now_, kFieldFresh);
    o.callsign = ks.callsign.has ? std::optional(ks.callsign.value) : std::nullopt;  // identity: lives with the key
    o.squawk = ks.squawk.has ? std::optional(ks.squawk.value) : std::nullopt;  // identity: lives with the key
    o.emergency = ks.emergency.has ? std::optional(ks.emergency.value) : std::nullopt;  // identity: lives with the key
    o.category = ks.category.has ? std::optional(ks.category.value) : std::nullopt;  // identity: lives with the key
    o.nic = ks.nic.fresh(now_, kFieldFresh);
    o.nac_p = ks.nac_p.fresh(now_, kFieldFresh);
    o.rssi = rssiOf(ks);
    o.raw_line = "-" + coding::toHex(f.bytes, true) + ";";
    o.input_id = input_id_;
    o.rebroadcast = ks.key.rebroadcast;
    ks.dirty = false;
    ++counters_.observations;
    handler_(o);
}

}  // namespace skycued::decode
