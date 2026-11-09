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
using namespace limits;

constexpr seconds kFieldFresh{60};      // our choice: observations carry fields <= 60 s old
constexpr seconds kAcasRaFresh{30};     // spec 14 item 8: an RA message keeps the field for 30 s
constexpr seconds kConfirmFresh{60};    // our choice: an address stays confirmed for 60 s
constexpr seconds kAirbornePair{10};    // our choice: airborne even/odd pair window
constexpr seconds kSurfacePair{30};     // our choice: surface pair window
constexpr seconds kSurfacePairSlow{60}; // our choice: surface pair window when the last gs <= 25 kt
constexpr float kSlowGsKt = 25.0f;
constexpr seconds kLocalLastPos{60};    // design 5.5 step 2: last position usable for a local decode; our choice, set by black-box comparison with the readsb golden (tests/test_decode_golden.cpp)
constexpr seconds kSurfaceRefAge{300};  // design 5.5 step 1: last position usable as surface reference
constexpr double kLocalRefNm = 180.0;   // our choice: reference-based local decode range (airborne)
constexpr seconds kDerivedBaroAge{10};  // design 5.4: baro/GNSS altitude ages for the TC19 difference

double secs(TimePoint a, TimePoint b) { return std::chrono::duration<double>(a - b).count(); }

// Sets a timed field; flags a change when it replaced a different value.
template <typename T>
void setField(KeyState& ks, Timed<T>& f, const T& v, TimePoint now, bool track) {
    if (track && f.has && !(f.value == v)) ks.gate.dirty = true;
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
    if (signal) ks->rssi.pushPower(RssiRing::fromBeastByte(*signal));
    if (capability(f) == 4) ks->on_ground.set(true, now_);
    ++counters_.decoded;  // no observation from a DF11 (design 5.2)
}


void ModesDecoder::applyAcasRa(KeyState& ks, const AcasRa& ra) {
    AcasRaState st;
    st.active = !ra.rat && !(!(ra.ara & 0x2000) && !ra.mte);  // ARA bit 1 clear and MTE clear: no vertical RA
    st.ara = ra.ara;
    st.rac = ra.rac;
    st.rat = ra.rat;
    st.mte = ra.mte;
    st.tti = ra.tti;
    st.threat_address = ra.threat_address;
    st.threat_alt_ft = ra.threat_alt_ft;
    st.threat_range_nm = ra.threat_range_nm;
    st.threat_bearing_deg = ra.threat_bearing_deg;
    st.text = ra.rat ? "clear of conflict" : acasRaText(ra);
    st.at = now_;
    // A change of advisory, or the return of one after the field expired, is sent at once (like emergency).
    const auto prev = ks.acas_ra.fresh(now_, kAcasRaFresh);
    if (ra.rat && !prev) return;  // a RAT with no advisory to end: nothing to report
    if (!prev || prev->active != st.active || prev->ara != st.ara || prev->rac != st.rac ||
        prev->mte != st.mte)
        ks.gate.dirty = true;
    ks.acas_ra.set(st, now_);
}

void ModesDecoder::applyCommb(KeyState& ks, const Frame& f) {
    // A field an ADS-B message set in the last 60 s keeps that value (ADS-B outranks Comm-B).
    const auto setCommb = [this](Timed<float>& field, bool& from_commb, float v) {
        if (field.fresh(now_, kFieldFresh) && !from_commb) return;
        field.set(v, now_);
        from_commb = true;
    };
    CommbContext ctx;
    ctx.track_deg = ks.track.fresh(now_, kFieldFresh);
    ctx.gs_kt = ks.gs.fresh(now_, kFieldFresh);
    ctx.baro_rate_fpm = ks.baro_rate.has && now_ - ks.baro_rate.at <= kFieldFresh
                            ? std::optional<int32_t>(static_cast<int32_t>(std::lround(ks.baro_rate.value)))
                            : std::nullopt;
    ctx.alt_ft = ks.alt_baro.fresh(now_, kFieldFresh);
    const CommbResult r = inferCommb(f.field56(32), ctx);
    switch (r.kind) {
    case CommbKind::bds40:
        ++counters_.commb_bds40;
        if (r.b40) {
            // Intent fields mark the key dirty when the value changes (like identity). A TC29
            // value under 60 s old outranks BDS 4,0; a later TC29
            // replaces a 4,0 value at once.
            const auto setIntent = [&](auto& field, bool& from_commb, const auto& v) {
                if (field.fresh(now_, kFieldFresh) && !from_commb) return;
                setField(ks, field, v, now_, true);
                from_commb = true;
            };
            if (r.b40->sel_alt_mcp_ft) setIntent(ks.sel_alt, ks.sel_alt_commb, *r.b40->sel_alt_mcp_ft);
            if (r.b40->sel_alt_fms_ft) setIntent(ks.sel_alt_fms, ks.sel_alt_fms_commb, *r.b40->sel_alt_fms_ft);
            if (r.b40->qnh_mb) setIntent(ks.qnh, ks.qnh_commb, *r.b40->qnh_mb);
            if (r.b40->modes_valid) {  // replaces the whole set, no merging (as readsb)
                uint8_t m = 0;
                if (r.b40->vnav) m |= kNavVnav;
                if (r.b40->althold) m |= kNavAltHold;
                if (r.b40->approach) m |= kNavApproach;
                setIntent(ks.nav_modes, ks.nav_modes_commb, m);
            }
        }
        break;
    case CommbKind::bds50:
        ++counters_.commb_bds50;
        if (r.b50) {
            if (r.b50->roll_deg) ks.roll.set(*r.b50->roll_deg, now_);
            if (r.b50->track_deg) setCommb(ks.track, ks.track_commb, *r.b50->track_deg);  // the ground track, as TC19 sets it
            if (r.b50->gs_kt) setCommb(ks.gs, ks.gs_commb, *r.b50->gs_kt);
            if (r.b50->track_rate_dps) ks.track_rate.set(*r.b50->track_rate_dps, now_);
            if (r.b50->tas_kt) ks.tas.set(*r.b50->tas_kt, now_);
        }
        break;
    case CommbKind::bds60:
        ++counters_.commb_bds60;
        if (r.b60) {
            if (r.b60->heading_mag_deg) ks.heading_mag.set(*r.b60->heading_mag_deg, now_);
            if (r.b60->ias_kt) ks.ias.set(*r.b60->ias_kt, now_);
            if (r.b60->mach) ks.mach.set(*r.b60->mach, now_);
            if (r.b60->baro_rate_fpm) setCommb(ks.baro_rate, ks.baro_rate_commb, static_cast<float>(*r.b60->baro_rate_fpm));
            if (r.b60->inertial_rate_fpm)
                setCommb(ks.geom_rate, ks.geom_rate_commb, static_cast<float>(*r.b60->inertial_rate_fpm));
        }
        break;
    case CommbKind::ambiguous: ++counters_.commb_ambiguous; break;
    case CommbKind::other: ++counters_.commb_other; break;
    case CommbKind::empty: ++counters_.commb_empty; break;
    case CommbKind::unknown: ++counters_.commb_unknown; break;
    }
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
    if (signal) ks->rssi.pushPower(RssiRing::fromBeastByte(*signal));

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
    if (f.df == 20 || f.df == 21) applyCommb(*ks, f);
    if (f.df == 0 || f.df == 16) {
        const AcasCommon c = decodeAcasCommon(f);
        ks->acas_sl.set(c.sl, now_);
        ks->acas_ri.set(c.ri, now_);
        if (f.df == 16) {
            if (const auto ra = decodeAcasRaMv(f)) {
                applyAcasRa(*ks, *ra);
                ++counters_.acas_ra;
            }
        }
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
    if (signal) ks.rssi.pushPower(RssiRing::fromBeastByte(*signal));
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
                ks.gs_commb = false;
                ks.gs.set(mv.gs_kt.value_or(0.0f), now_);
                ks.last_gs_kt = mv.gs_kt.value_or(0.0f);
            }
            if (mv.track_deg) {
                ks.track_commb = false;
                ks.track.set(*mv.track_deg, now_);
            }
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
                ks.gs_commb = false;
                ks.gs.set(v->gs_kt.value_or(0.0f), now_);
                ks.last_gs_kt = v->gs_kt.value_or(0.0f);
            }
            if (v->track_deg) {
                ks.track_commb = false;
                ks.track.set(*v->track_deg, now_);
            }
            if (v->heading_mag_deg) ks.heading_mag.set(*v->heading_mag_deg, now_);
            if (v->ias_kt) ks.ias.set(*v->ias_kt, now_);
            if (v->tas_kt) ks.tas.set(static_cast<float>(*v->tas_kt), now_);
            if (v->rate_fpm) {
                const float r = static_cast<float>(*v->rate_fpm);
                if (v->rate_is_geom) {
                    ks.geom_rate_commb = false;
                    ks.geom_rate.set(r, now_);
                } else {
                    ks.baro_rate_commb = false;
                    ks.baro_rate.set(r, now_);
                }
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
            if (em) setField(ks, ks.emergency, *em, now_, true);
            if (sq) setField(ks, ks.squawk, *sq, now_, true);
            ++counters_.decoded;
        } else if (const auto ra = decodeAcasRaTc28(f)) {
            applyAcasRa(ks, *ra);
            ++counters_.acas_ra_tc28;
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
        if (const auto t = decodeTargetState(f)) {
            if (t->sel_alt_ft) { setField(ks, ks.sel_alt, *t->sel_alt_ft, now_, true); ks.sel_alt_commb = false; }
            if (t->sel_alt_fms_ft) { setField(ks, ks.sel_alt_fms, *t->sel_alt_fms_ft, now_, true); ks.sel_alt_fms_commb = false; }
            if (t->qnh_mb) { setField(ks, ks.qnh, *t->qnh_mb, now_, true); ks.qnh_commb = false; }
            if (t->sel_heading_deg) setField(ks, ks.sel_heading, *t->sel_heading_deg, now_, true);
            // NACp stays TC31-only: taking the TC29 value drops nac_p agreement with readsb below its floor.
            if (t->modes_valid) {  // the flags mean nothing while the status bit is 0
                uint8_t m = 0;
                if (t->autopilot) m |= kNavAutopilot;
                if (t->vnav) m |= kNavVnav;
                if (t->althold) m |= kNavAltHold;
                if (t->approach) m |= kNavApproach;
                if (t->lnav) m |= kNavLnav;
                if (t->tcas) m |= kNavTcas;
                setField(ks, ks.nav_modes, m, now_, true);
                ks.nav_modes_commb = false;
            }
            ++counters_.decoded;
        } else {
            ++counters_.unmapped;
        }
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
        if (!withinRange(*cand, anchor)) ok = false;
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
    ks.gate.last_accept_at = now_;
    if (global) ks.need_global = false;
    out = *cand;
    return true;
}

void ModesDecoder::emit(KeyState& ks, const Frame& f, Clock::time_point arrival, const cpr::Position* pos) {
    if (!handler_) {
        if (pos) ks.gate.dirty = false;
        return;
    }
    if (!pos) {
        // Design 7.2: without a position in the last 10 s, a change of identity, squawk or
        // altitude emits, at most once per second; a change held back waits for the next frame.
        if (!shouldEmitWithoutPosition(ks.gate, now_)) return;
    }
    ks.gate.dirty = false;

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
    o.heading_mag_deg = ks.heading_mag.fresh(now_, kFieldFresh);
    o.heading_true_deg = ks.heading_true.fresh(now_, kFieldFresh);
    o.ias_kt = ks.ias.fresh(now_, kFieldFresh);
    o.tas_kt = ks.tas.fresh(now_, kFieldFresh);
    o.mach = ks.mach.fresh(now_, kFieldFresh);
    o.roll_deg = ks.roll.fresh(now_, kFieldFresh);
    o.track_rate_dps = ks.track_rate.fresh(now_, kFieldFresh);
    o.sel_alt_ft = ks.sel_alt.fresh(now_, kFieldFresh);
    o.sel_alt_fms_ft = ks.sel_alt_fms.fresh(now_, kFieldFresh);
    o.qnh_mb = ks.qnh.fresh(now_, kFieldFresh);
    o.sel_heading_deg = ks.sel_heading.fresh(now_, kFieldFresh);
    o.nav_modes = ks.nav_modes.fresh(now_, kFieldFresh);
    o.acas_ra = ks.acas_ra.fresh(now_, kAcasRaFresh);
    o.acas_sl = ks.acas_sl.fresh(now_, kFieldFresh);
    o.acas_ri = ks.acas_ri.fresh(now_, kFieldFresh);
    o.baro_rate_fpm = ks.baro_rate.fresh(now_, kFieldFresh);
    o.geom_rate_fpm = ks.geom_rate.fresh(now_, kFieldFresh);
    o.callsign = ks.callsign.has ? std::optional(ks.callsign.value) : std::nullopt;  // identity: lives with the key
    o.squawk = ks.squawk.has ? std::optional(ks.squawk.value) : std::nullopt;  // identity: lives with the key
    o.emergency = ks.emergency.has ? std::optional(ks.emergency.value) : std::nullopt;  // identity: lives with the key (spec 14 item 5)
    o.category = ks.category.has ? std::optional(ks.category.value) : std::nullopt;  // identity: lives with the key
    o.nic = ks.nic.fresh(now_, kFieldFresh);
    o.nac_p = ks.nac_p.fresh(now_, kFieldFresh);
    o.rssi = ks.rssi.meanDb();
    o.raw_line = "*" + coding::toHex(f.bytes, true) + ";";
    o.input_id = input_id_;
    o.rebroadcast = ks.key.rebroadcast;
    ++counters_.observations;
    handler_(o);
}

}  // namespace skycued::decode
