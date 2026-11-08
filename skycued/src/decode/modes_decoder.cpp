#include "decode/modes_decoder.hpp"

#include <algorithm>
#include <cmath>

#include "coding/bits.hpp"
#include "coding/modes_crc.hpp"

// Rules: frame decoder design sections 5.2 (CRC and address), 5.3 (message
// types), 5.4 (altitude), 5.5 (CPR), 5.6 (DF18 control field) and 7
// (observation production). Constants marked "our choice" are not cited values.

namespace skycued::decode {

namespace {

using std::chrono::seconds;
using skycue::AddressType;
using skycue::Source;

constexpr seconds kFieldFresh{60};      // our choice: observations carry fields <= 60 s old
constexpr seconds kConfirmFresh{60};    // our choice: an address stays confirmed for 60 s
constexpr seconds kAirbornePair{10};    // our choice: airborne even/odd pair window
constexpr seconds kSurfacePair{30};     // our choice: surface pair window
constexpr seconds kSurfacePairSlow{60}; // our choice: surface pair window when the last gs <= 25 kt
constexpr float kSlowGsKt = 25.0f;
constexpr seconds kLocalLastPos{60};    // design 5.5 step 2: last position usable for a local decode; our choice, set by black-box comparison with the readsb golden (tests/test_decode_golden.cpp)
constexpr seconds kSurfaceRefAge{300};  // design 5.5 step 1: last position usable as surface reference
constexpr double kLocalRefNm = 180.0;   // our choice: reference-based local decode range (airborne)
constexpr double kRangeNm = 300.0;      // our choice: reasonableness range from the reference/first position
constexpr double kSpeedBaseNm = 0.5;    // our choice: speed check 0.5 NM + elapsed x max(1.5 gs, 150 kt)
constexpr double kSpeedMinKt = 150.0;   // our choice
constexpr double kSpeedCapAirKt = 1200.0;    // our choice
constexpr double kSpeedCapSurfaceKt = 100.0;  // our choice
constexpr seconds kDerivedBaroAge{10};  // design 5.4: baro/GNSS altitude ages for the TC19 difference
constexpr seconds kNoPositionFor{10};   // design 7.2: change emissions only without a position this recent
constexpr seconds kChangeInterval{1};   // design 7.2: at most one change emission per second
constexpr std::size_t kRssiFrames = 8;  // our choice: rssi is the mean over the last 8 frames

double secs(TimePoint a, TimePoint b) { return std::chrono::duration<double>(a - b).count(); }

void pushRssi(KeyState& ks, uint8_t s) {
    ks.rssi_ring[ks.rssi_next] = s;
    ks.rssi_next = (ks.rssi_next + 1) % kRssiFrames;
    if (ks.rssi_count < kRssiFrames) ++ks.rssi_count;
}

std::optional<double> rssiOf(const KeyState& ks) {
    if (ks.rssi_count == 0) return std::nullopt;
    double sum = 0.0;
    for (std::size_t i = 0; i < ks.rssi_count; ++i) {
        const double a = ks.rssi_ring[i] / 255.0;
        sum += a * a;
    }
    const double mean = sum / static_cast<double>(ks.rssi_count);
    if (mean <= 0.0) return std::nullopt;  // no level at all
    return std::round(10.0 * std::log10(mean) * 10.0) / 10.0;
}

// Sets a timed field; flags a change when it replaced a different value.
template <typename T>
void setField(KeyState& ks, Timed<T>& f, const T& v, TimePoint now, bool track) {
    if (track && f.has && !(f.value == v)) ks.dirty = true;
    f.set(v, now);
}

}  // namespace

ModesDecoder::ModesDecoder(DecoderConfig cfg, std::string input_id)
    : cfg_(cfg), input_id_(std::move(input_id)) {}

void ModesDecoder::onObservation(std::function<void(const TrackObservation&)> h) { handler_ = std::move(h); }
const DecodeCounters& ModesDecoder::counters() const { return counters_; }
std::size_t ModesDecoder::keys() const { return keys_.size(); }

KeyState* ModesDecoder::find(const StateKey& k) {
    const auto it = keys_.find(k);
    return it == keys_.end() ? nullptr : &it->second;
}

KeyState& ModesDecoder::obtain(const StateKey& k, Source src) {
    KeyState* ks = find(k);
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

void ModesDecoder::evictIdle() {
    while (!lru_.empty()) {
        const auto it = keys_.find(lru_.front());
        if (it != keys_.end() && now_ - it->second.last_seen <= cfg_.idle_evict) break;
        if (it != keys_.end()) keys_.erase(it);
        lru_.pop_front();
        ++counters_.evicted;
    }
}

void ModesDecoder::feed(const std::vector<uint8_t>& bytes, Clock::time_point arrival,
                        std::optional<uint8_t> signal) {
    ++counters_.frames;
    frame_ground_.reset();
    auto f = classify(bytes);
    if (!f) {
        ++counters_.unknown_type;
        return;
    }
    // Max-so-far time: arrival times may step back, windows never do.
    if (!have_now_ || arrival > now_) {
        now_ = arrival;
        have_now_ = true;
    }
    evictIdle();

    switch (f->df) {
    case 17:
    case 18: {
        const uint32_t residual = coding::modesCrcResidual(f->bytes);
        if (residual != 0) {
            std::vector<uint8_t> fixed = f->bytes;
            if (!coding::syndromeFix1(fixed)) {
                ++counters_.crc_fail;
                return;
            }
            ++counters_.crc_fixed;
            auto g = classify(fixed);
            if (!g) {  // cannot happen: the correction never touches the DF bits
                ++counters_.crc_fail;
                return;
            }
            f = std::move(g);
        }
        StateKey key;
        key.address = (uint32_t{f->bytes[1]} << 16) | (uint32_t{f->bytes[2]} << 8) | f->bytes[3];
        Source src = Source::adsb_1090;
        if (f->df == 18) {
            // Control field table, design section 5.6.
            const unsigned cf = f->bits(5, 3);
            // Where the IMF bit sits is not in the public sources we have (design
            // section 12): ME bit 8, the single-antenna flag's place in ADS-B. For surface
            // position messages that bit is also a movement bit, so an IMF read there is
            // unreliable until the differential test or a DF18 capture settles it.
            const bool imf = f->bits(39, 1) != 0;
            switch (cf) {
            case 0: break;
            case 1: key.type = AddressType::non_icao; break;
            case 2:
                key.rebroadcast = true;
                key.type = imf ? AddressType::tisb_trackfile : AddressType::icao;
                src = Source::tisb;
                break;
            case 5:
                key.rebroadcast = true;
                key.type = AddressType::non_icao;
                src = Source::tisb;
                break;
            case 6:
                key.rebroadcast = true;
                key.type = imf ? AddressType::non_icao : AddressType::icao;
                src = Source::uat_978;
                break;
            default:  // 3, 4, 7: coarse TIS-B, management, reserved
                ++counters_.unknown_type;
                return;
            }
        }
        onEs(*f, key, src, arrival, signal);
        return;
    }
    case 11:
        onDf11(*f, coding::modesCrcResidual(f->bytes), signal);
        return;
    default:
        onAp(*f, arrival, signal);
        return;
    }
}

void ModesDecoder::onDf11(const Frame& f, uint32_t residual, std::optional<uint8_t> signal) {
    StateKey key;
    key.address = (uint32_t{f.bytes[1]} << 16) | (uint32_t{f.bytes[2]} << 8) | f.bytes[3];
    KeyState* ks = nullptr;
    if (residual == 0) {
        ks = &obtain(key, Source::adsb_1090);
    } else if (residual <= 79) {
        // An interrogator id in the parity only refreshes an address that is already confirmed.
        ks = find(key);
        if (ks && ks->confirmed.fresh(now_, kConfirmFresh)) {
            ks = &obtain(key, ks->source);
        } else {
            ks = nullptr;
        }
    }
    if (!ks) {
        ++counters_.crc_fail;
        return;
    }
    ks->confirmed.set(true, now_);
    if (signal) pushRssi(*ks, *signal);
    if (capability(f) == 4) ks->on_ground.set(true, now_);
    ++counters_.decoded;  // no observation from a DF11 (design 5.2)
}

void ModesDecoder::onAp(const Frame& f, Clock::time_point arrival, std::optional<uint8_t> signal) {
    const uint32_t residual = coding::modesCrcResidual(f.bytes);  // the AP field: the address
    StateKey key;
    key.address = residual & 0xFFFFFFu;
    KeyState* ks = find(key);
    if (!ks || !ks->confirmed.fresh(now_, kConfirmFresh)) {
        ++counters_.ap_unconfirmed;
        return;
    }
    ks = &obtain(key, ks->source);
    if (signal) pushRssi(*ks, *signal);

    switch (f.df) {
    case 0:
    case 16:
    case 4:
    case 20: {
        if (const auto alt = altitudeAc13(f.bits(19, 13))) {
            const bool ground = ks->on_ground.fresh(now_, kFieldFresh).value_or(false);
            setField(*ks, ks->alt_baro, *alt, now_, !ground);
        }
        break;
    }
    default: {  // 5, 21
        if (const auto sq = squawkId13(f.bits(19, 13))) setField(*ks, ks->squawk, *sq, now_, true);
        break;
    }
    }
    if (f.df == 0 || f.df == 16) {
        if (verticalStatusGround(f)) setGround(*ks, true);
    } else {
        const int fs = flightStatus(f);
        if (fs == 1 || fs == 3) setGround(*ks, true);
        else if (fs == 0 || fs == 2) setGround(*ks, false);
    }
    ++counters_.decoded;
    emit(*ks, f, arrival, nullptr);
}

void ModesDecoder::onEs(const Frame& f, const StateKey& key, Source src, Clock::time_point arrival,
                        std::optional<uint8_t> signal) {
    KeyState& ks = obtain(key, src);
    // Confirmation: clean/fixed DF17 and DF18 CF 0 only; DF18 CF 1 is the non-ICAO key and
    // the rebroadcast classes have their own keys, which no AP reply ever looks up.
    if (f.df == 17 || (f.df == 18 && f.bits(5, 3) == 0)) ks.confirmed.set(true, now_);
    if (signal) pushRssi(ks, *signal);
    if (f.df == 17 && capability(f) == 4) setGround(ks, true);

    const int tc = typeCode(f);
    std::optional<cpr::Position> accepted;

    if (tc == 0) {
        ++counters_.no_position;
    } else if (tc >= 1 && tc <= 4) {
        setField(ks, ks.category, std::string(1, "DCBA"[tc - 1]) + std::to_string(f.bits(37, 3)), now_, true);
        const std::string cs = decodeCallsignIdent(f);
        if (!cs.empty()) setField(ks, ks.callsign, cs, now_, true);  // any invalid char: dropped
        ++counters_.decoded;
    } else if ((tc >= 5 && tc <= 8) || (tc >= 9 && tc <= 18) || (tc >= 20 && tc <= 22)) {
        const auto c = decodeCpr(f);
        if (!c) {  // cannot happen for these type codes; do not rely on that
            ++counters_.unmapped;
            emit(ks, f, arrival, nullptr);
            return;
        }
        const bool surface = c->surface;
        if (surface) {
            const SurfaceMovement mv = decodeSurfaceMovement(f);
            if (mv.gs_kt) {
                ks.gs.set(mv.gs_kt.value_or(0.0f), now_);
                ks.last_gs_kt = mv.gs_kt.value_or(0.0f);
            }
            if (mv.track_deg) ks.track.set(*mv.track_deg, now_);
            setGround(ks, true);
        } else {
            // TC 9-18: barometric AC12 (Q bit). TC 20-22: GNSS height in metres, no Q bit.
            if (tc <= 18) {
                if (const auto alt = altitudeAc12(f.bits(40, 12))) setField(ks, ks.alt_baro, *alt, now_, true);
            } else if (const auto alt = altitudeGnssHeightM12(f.bits(40, 12))) {
                setField(ks, ks.alt_gnss, *alt, now_, true);
            }
            setGround(ks, false);
        }
        const bool b = surface ? ks.tc31.nic_c : f.bits(39, 1) != 0;
        const int version = ks.tc31.seen ? ks.tc31.version : 0;
        if (const auto nic = nicFor(tc, version, ks.tc31.nic_a, b, surface)) ks.nic.set(*nic, now_);
        ++counters_.decoded;
        cpr::Position p{};
        if (positionStep(ks, *c, p)) accepted = p;
    } else if (tc == 19) {
        if (const auto v = decodeVelocity(f)) {
            if (v->gs_kt) {
                ks.gs.set(v->gs_kt.value_or(0.0f), now_);
                ks.last_gs_kt = v->gs_kt.value_or(0.0f);
            }
            if (v->track_deg) ks.track.set(*v->track_deg, now_);
            if (v->rate_fpm) {
                const float r = static_cast<float>(*v->rate_fpm);
                if (v->rate_is_geom) ks.geom_rate.set(r, now_);
                else ks.baro_rate.set(r, now_);
            }
            if (v->gnss_baro_diff_ft) ks.gnss_diff.set(*v->gnss_baro_diff_ft, now_);
            // GNSS minus baro difference (design 5.4): only from a fresh baro altitude
            // and when no TC20-22 altitude is fresh.
            if (v->gnss_baro_diff_ft && ks.alt_baro.fresh(now_, kDerivedBaroAge) &&
                !ks.alt_gnss.fresh(now_, kDerivedBaroAge)) {
                ks.alt_derived.set(ks.alt_baro.value + *v->gnss_baro_diff_ft, now_);
            }
            ++counters_.decoded;
        } else {
            ++counters_.unmapped;
        }
    } else if (tc == 28) {
        const auto em = decodeEmergency(f);
        const auto sq = decodeSquawkTc28(f);
        if (em || sq) {
            if (em) setField(ks, ks.emergency, *em, now_, false);
            if (sq) setField(ks, ks.squawk, *sq, now_, true);
            ++counters_.decoded;
        } else {
            ++counters_.unmapped;  // ST 2 and others: decoded, no field
        }
    } else if (tc == 31) {
        if (const auto o = decodeOperationalStatus(f)) {
            ks.tc31 = {o->version, o->nic_a, o->nic_c, true};
            if (o->version > 0 && o->nac_p) ks.nac_p.set(*o->nac_p, now_);
            ++counters_.decoded;
        } else {
            ++counters_.unmapped;
        }
    } else if (tc == 29) {
        ++counters_.unmapped;
    } else {  // 23-27, 30
        ++counters_.unknown_type;
    }

    emit(ks, f, arrival, accepted ? &*accepted : nullptr);
}

void ModesDecoder::setGround(KeyState& ks, bool g) {
    ks.on_ground.set(g, now_);
    frame_ground_ = g;
}

void ModesDecoder::reject(KeyState& ks) {
    // Design 5.5 step 5: the position and its frame are dropped, the last position and all
    // stored halves of both kinds cleared, and only a fresh global pair may restart the key.
    ++counters_.cpr_rejected;
    ks.last_pos.reset();
    for (auto& kind : ks.halves)
        for (auto& h : kind) h.has = false;
    ks.need_global = true;
}

bool ModesDecoder::positionStep(KeyState& ks, const CprFields& c, cpr::Position& out) {
    const int kind = c.surface ? 1 : 0;
    // Step 6: a kind change discards the other kind's stored messages.
    if (ks.cpr_kind != -1 && ks.cpr_kind != kind)
        for (auto& h : ks.halves[1 - kind]) h.has = false;
    ks.cpr_kind = kind;

    const cpr::Msg m{c.lat17, c.lon17, c.odd};
    CprHalf& mine = ks.halves[kind][c.odd ? 1 : 0];
    mine = CprHalf{m, now_, true};
    const CprHalf& other = ks.halves[kind][c.odd ? 0 : 1];

    // The surface window grows to 60 s for a slow target (our choice).
    seconds window = kAirbornePair;
    if (c.surface) window = (ks.last_gs_kt && *ks.last_gs_kt <= kSlowGsKt) ? kSurfacePairSlow : kSurfacePair;

    auto localDecode = [&]() -> std::optional<cpr::Position> {
        if (ks.need_global) return std::nullopt;  // step 5: no local decode until a global fix
        if (ks.last_pos && now_ - ks.last_pos_at <= kLocalLastPos) return cpr::local(m, *ks.last_pos, c.surface);
        if (!c.surface && cfg_.reference) {
            const auto p = cpr::local(m, *cfg_.reference, false);
            if (p && cpr::distanceNm(*p, *cfg_.reference) <= kLocalRefNm) return p;
        }
        return std::nullopt;
    };

    std::optional<cpr::Position> cand;
    bool global = false;
    const bool paired = other.has && now_ - other.at <= window;
    if (paired) {
        const cpr::Msg& even = c.odd ? other.msg : m;
        const cpr::Msg& odd = c.odd ? m : other.msg;
        if (!c.surface) {
            cand = cpr::globalAirborne(even, odd, c.odd);
        } else {
            std::optional<cpr::Position> ref;
            if (ks.last_pos && now_ - ks.last_pos_at <= kSurfaceRefAge) ref = ks.last_pos;
            else ref = cfg_.reference;
            if (!ref) {
                reject(ks);  // surface global needs a reference to pick the quadrant
                return false;
            }
            cand = cpr::globalSurface(even, odd, c.odd, *ref);
        }
        global = cand.has_value();
        if (!cand) {
            // The pair straddles an NL boundary: the global rule fails. A local decode may still
            // apply (step 2); otherwise it waits (step 3), it is not a rejection.
            cand = localDecode();
            if (!cand) {
                // Step 3: it decodes by neither rule and waits for a new partner. The
                // newer half stays stored, the older one is dropped; no rejection.
                ++counters_.cpr_pending;
                ks.halves[kind][c.odd ? 0 : 1].has = false;
                return false;
            }
        }
    } else {
        cand = localDecode();
        if (!cand) {
            ++counters_.cpr_pending;  // step 3: waits for its pair
            return false;
        }
    }

    // Step 4: reasonableness (our own constants).
    bool ok = std::isfinite(cand->lat) && std::isfinite(cand->lon) && std::fabs(cand->lat) <= 90.0;
    if (ok) {
        const std::optional<cpr::Position> anchor = cfg_.reference ? cfg_.reference : ks.first_pos;
        if (anchor && cpr::distanceNm(*cand, *anchor) > kRangeNm) ok = false;
    }
    if (ok && ks.last_pos) {
        const double elapsed = std::max(0.0, secs(now_, ks.last_pos_at));
        const double gs = ks.last_gs_kt.value_or(0.0f);
        const double kt = std::min(std::max(1.5 * gs, kSpeedMinKt), c.surface ? kSpeedCapSurfaceKt : kSpeedCapAirKt);
        if (cpr::distanceNm(*cand, *ks.last_pos) > kSpeedBaseNm + elapsed * kt / 3600.0) ok = false;
    }
    if (!ok) {
        reject(ks);
        return false;
    }
    ks.last_pos = cand;
    ks.last_pos_at = now_;
    if (!ks.first_pos) ks.first_pos = cand;
    ks.last_accept_at = now_;
    if (global) ks.need_global = false;
    out = *cand;
    return true;
}

void ModesDecoder::emit(KeyState& ks, const Frame& f, Clock::time_point arrival, const cpr::Position* pos) {
    if (!handler_) {
        if (pos) ks.dirty = false;
        return;
    }
    if (!pos) {
        // Design 7.2: without a position in the last 10 s, a change of identity, squawk or
        // altitude emits, at most once per second; a change held back waits for the next frame.
        if (!ks.dirty) return;
        if (ks.last_accept_at && now_ - *ks.last_accept_at <= kNoPositionFor) return;
        if (ks.last_change_emit && now_ - *ks.last_change_emit < kChangeInterval) return;
        ks.last_change_emit = now_;
    }
    ks.dirty = false;

    TrackObservation o;
    o.id.address = ks.key.address;
    o.id.type = ks.key.type;
    o.source = ks.source;
    o.received_time = arrival;
    if (pos) {
        o.lat = pos->lat;
        o.lon = pos->lon;
        o.position_time = arrival;
    }
    // on_ground is asserted only by the triggering frame (design 7.1); the stored state
    // (up to 60 s old) still decides whether the barometric altitude is suppressed.
    const auto ground = ks.on_ground.fresh(now_, kFieldFresh);
    o.on_ground = frame_ground_;
    if (!ground.value_or(false)) o.alt_baro_ft = ks.alt_baro.fresh(now_, kFieldFresh);  // ground suppresses it
    if (o.alt_baro_ft) o.alt_baro_time = ks.alt_baro.at;
    // The difference carries its own time (geom_minus_baro_time) whenever it is emitted.
    o.alt_geom_ft = ks.alt_gnss.fresh(now_, kFieldFresh);
    if (o.alt_geom_ft) {
        o.alt_geom_time = ks.alt_gnss.at;
        // A TC20-22 height pairs only with a recent difference; the baro is not its basis.
        o.geom_minus_baro_ft = ks.gnss_diff.fresh(now_, kDerivedBaroAge);
    } else {
        o.geom_minus_baro_ft = ks.gnss_diff.fresh(now_, kFieldFresh);  // also when the baro was too old to derive a geom
        const auto derived = ks.alt_derived.fresh(now_, kFieldFresh);
        // A newer TC19 difference whose baro was too old to re-derive supersedes the derived
        // value: emit the difference alone, with its own time.
        const bool superseded = o.geom_minus_baro_ft && ks.gnss_diff.at > ks.alt_derived.at;
        if (derived && !superseded) {
            o.alt_geom_ft = derived;
            o.alt_geom_time = ks.alt_derived.at;
        }
    }
    if (o.geom_minus_baro_ft) o.geom_minus_baro_time = ks.gnss_diff.at;
    o.gs_kt = ks.gs.fresh(now_, kFieldFresh);
    o.track_deg = ks.track.fresh(now_, kFieldFresh);
    o.baro_rate_fpm = ks.baro_rate.fresh(now_, kFieldFresh);
    o.geom_rate_fpm = ks.geom_rate.fresh(now_, kFieldFresh);
    o.callsign = ks.callsign.has ? std::optional(ks.callsign.value) : std::nullopt;  // identity: lives with the key
    o.squawk = ks.squawk.has ? std::optional(ks.squawk.value) : std::nullopt;  // identity: lives with the key
    o.emergency = ks.emergency.fresh(now_, kFieldFresh);
    o.category = ks.category.has ? std::optional(ks.category.value) : std::nullopt;  // identity: lives with the key
    o.nic = ks.nic.fresh(now_, kFieldFresh);
    o.nac_p = ks.nac_p.fresh(now_, kFieldFresh);
    o.rssi = rssiOf(ks);
    o.raw_line = "*" + coding::toHex(f.bytes, true) + ";";
    o.input_id = input_id_;
    o.rebroadcast = ks.key.rebroadcast;
    ++counters_.observations;
    handler_(o);
}

}  // namespace skycued::decode
