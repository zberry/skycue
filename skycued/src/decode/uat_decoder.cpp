#include "decode/uat_decoder.hpp"

#include <algorithm>
#include <cmath>

#include "coding/bits.hpp"

// Rules: decoder spec sections 5.4 (altitude), 6 (address qualifiers) and 7
// (observation production). Position: only the 300 NM range check of 5.5 step 4, not its speed
// check. UAT positions are plain lat/lon (no CPR), so there is no decode ambiguity for a speed
// check to catch; Reed-Solomon coding makes an undetected bit error in a position vanishingly
// rare; and a speed gate on 978 only rejects fixes the reference decoder accepts. Constants marked "our choice" are not cited values.

namespace skycued::decode {

namespace {

using std::chrono::seconds;
using skycue::AddressType;
using skycue::Source;

// Sets a timed field; flags a change when it is new, expired or replaced a different value.
template <typename T>
void setField(UatKeyState& ks, Timed<T>& f, const T& v, TimePoint now) {
    if (!f.fresh(now, limits::kFieldFresh) || !(f.value == v)) ks.gate.dirty = true;
    f.set(v, now);
}

// Identity fields live with the key (spec 14 item 5): changed when new or different, never expired.
template <typename T>
void setIdentity(UatKeyState& ks, Timed<T>& f, const T& v, TimePoint now) {
    if (!f.has || !(f.value == v)) ks.gate.dirty = true;
    f.set(v, now);
}

}  // namespace

UatDecoder::UatDecoder(DecoderConfig cfg, std::string input_id)
    : cfg_(cfg), input_id_(std::move(input_id)), keys_(cfg.max_keys, cfg.idle_evict, counters_.evicted) {}

void UatDecoder::onObservation(std::function<void(const skycued::TrackObservation&)> h) { handler_ = std::move(h); }
const DecodeCounters& UatDecoder::counters() const { return counters_; }
std::size_t UatDecoder::keys() const { return keys_.size(); }

void UatDecoder::feed(const uat::UatFrame& f, Clock::time_point arrival, std::optional<double> rssi_dbfs) {
    ++counters_.frames;
    if (f.rs_errors && *f.rs_errors > 0) counters_.rs_errors += static_cast<uint64_t>(*f.rs_errors);
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
    keys_.evictIdle(now_);

    StateKey key;
    key.address = f.address();
    key.type = map->type;
    key.rebroadcast = map->rebroadcast;
    UatKeyState& ks = keys_.obtain(key, map->source, now_);
    const auto level = rssi_dbfs ? rssi_dbfs : f.rssi_dbfs;
    if (level && std::isfinite(*level)) ks.rssi.pushPower(RssiRing::fromDbfs(*level));

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
    const bool ground = ks.on_ground.fresh(now_, limits::kFieldFresh).value_or(false);

    if (baro) {
        if (ground) ks.alt_baro.set(*baro, now_);  // stored, but a ground target does not report it
        else setField(ks, ks.alt_baro, *baro, now_);
    }
    if (geom) setField(ks, ks.alt_geom, *geom, now_);
    if (sv.gs_kt) setField(ks, ks.gs, *sv.gs_kt, now_);
    if (sv.track_deg) setField(ks, ks.track, *sv.track_deg, now_);
    if (sv.heading_mag_deg) ks.heading_mag.set(*sv.heading_mag_deg, now_);
    if (sv.heading_true_deg) ks.heading_true.set(*sv.heading_true_deg, now_);
    if (sv.vrate_fpm) {
        const float r = static_cast<float>(*sv.vrate_fpm);
        Timed<float>& mine = sv.vrate_geometric.value_or(false) ? ks.geom_rate : ks.baro_rate;
        Timed<float>& other = sv.vrate_geometric.value_or(false) ? ks.baro_rate : ks.geom_rate;
        setField(ks, mine, r, now_);
        if (other.has) {  // the source flipped: the other source's rate is stale, not carried for 60 s
            other = Timed<float>{};
            ks.gate.dirty = true;
        }
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
        bool ok = std::isfinite(p.lat) && std::isfinite(p.lon) && std::fabs(p.lat) <= 90.0 &&
                  withinRange(p, anchor);
        if (!ok) {
            ++counters_.cpr_rejected;  // the fix is dropped; no stored position is touched
            dropped = true;
        } else {
            if (!ks.first_pos) ks.first_pos = p;
            ks.gate.last_accept_at = now_;
            accepted = p;  // every accepted in-range fix is new, even a repeat: position_time must advance
        }
    }

    // Counters: one of unknown_type (above), no_position, unmapped, decoded per frame.
    if (!sv.lat || !sv.lon || dropped) ++counters_.no_position;
    else if (f.payload_type() >= 7) ++counters_.unmapped;  // target state / trajectory change content is not decoded
    else ++counters_.decoded;

    if (!handler_) {
        if (accepted) ks.gate.dirty = false;
        return;
    }
    if (!accepted) {
        // Spec 7.2: without an accepted position in the last 10 s, a change emits, at most once
        // per second; a change held back waits for the next frame (dirty stays set).
        if (!shouldEmitWithoutPosition(ks.gate, now_)) return;
    }

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
    if (!ground) o.alt_baro_ft = ks.alt_baro.fresh(now_, limits::kFieldFresh);  // ground suppresses it (spec 5.4)
    if (o.alt_baro_ft) o.alt_baro_time = ks.alt_baro.at;
    o.alt_geom_ft = ks.alt_geom.fresh(now_, limits::kFieldFresh);
    if (o.alt_geom_ft) o.alt_geom_time = ks.alt_geom.at;
    if (frame_diff && o.alt_baro_ft && o.alt_geom_ft) {  // only a difference this frame measured
        o.geom_minus_baro_ft = frame_diff;
        o.geom_minus_baro_time = now_;
    }
    fillCommon(o, ks, now_);
    o.raw_line = "-" + coding::toHex(f.bytes, true) + ";";
    o.input_id = input_id_;
    o.rebroadcast = ks.key.rebroadcast;
    ks.gate.dirty = false;
    ++counters_.observations;
    handler_(o);
}

}  // namespace skycued::decode
