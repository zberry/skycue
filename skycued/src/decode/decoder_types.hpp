#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "decode/cpr.hpp"

namespace skycued::decode {

using TimePoint = std::chrono::system_clock::time_point;

// Reasonableness and emission limits shared by the 1090 and 978 decoders (decoder spec 5.5 step 4, 7.2).
namespace limits {
constexpr double kRangeNm = 300.0;            // our choice: reasonableness range from the reference/first position
constexpr double kSpeedBaseNm = 0.5;          // our choice: speed check 0.5 NM + elapsed x max(1.5 gs, 150 kt)
constexpr double kSpeedMinKt = 150.0;         // our choice
constexpr double kSpeedCapAirKt = 1200.0;     // our choice
constexpr double kSpeedCapSurfaceKt = 100.0;  // our choice
constexpr std::chrono::seconds kNoPositionFor{10};  // design 7.2: change emissions only without a position this recent
constexpr std::chrono::seconds kChangeInterval{1};  // design 7.2: at most one change emission per second
}  // namespace limits

// Mean signal level over the last kRssiFrames frames that carried one (our choice). Both decoders push linear power.
constexpr std::size_t kRssiFrames = 8;

class RssiRing {
public:
    void pushPower(double power) {
        ring_[next_] = power;
        next_ = (next_ + 1) % kRssiFrames;
        if (count_ < kRssiFrames) ++count_;
    }
    // nullopt: no frame, or a mean that is not positive and finite; else dB to the tenth.
    std::optional<double> meanDb() const {
        if (count_ == 0) return std::nullopt;
        double sum = 0.0;
        for (std::size_t i = 0; i < count_; ++i) sum += ring_[i];
        const double mean = sum / static_cast<double>(count_);
        if (!(mean > 0.0) || !std::isfinite(mean)) return std::nullopt;
        return std::round(10.0 * std::log10(mean) * 10.0) / 10.0;
    }
    static double fromBeastByte(uint8_t s) {
        const double a = s / 255.0;
        return a * a;
    }
    static double fromDbfs(double dbfs) { return std::pow(10.0, dbfs / 10.0); }

private:
    std::array<double, kRssiFrames> ring_{};
    std::size_t count_ = 0, next_ = 0;
};

// Change-emission state of one key (design 7.2).
struct EmitGate {
    bool dirty = false;  // a tracked field was first seen or changed and is not yet emitted
    std::optional<TimePoint> last_accept_at;    // last accepted position (survives the clearing of last_pos)
    std::optional<TimePoint> last_change_emit;  // last observation emitted without a position
};

// A change without a position emits only when dirty, more than limits::kNoPositionFor after the last accepted
// position, and at least limits::kChangeInterval after the last such emission; true records last_change_emit.
inline bool shouldEmitWithoutPosition(EmitGate& g, TimePoint now) {
    if (!g.dirty) return false;
    if (g.last_accept_at && now - *g.last_accept_at <= limits::kNoPositionFor) return false;
    if (g.last_change_emit && now - *g.last_change_emit < limits::kChangeInterval) return false;
    g.last_change_emit = now;
    return true;
}

// false when an anchor is set and p is farther than limits::kRangeNm from it (exactly kRangeNm is within).
inline bool withinRange(const cpr::Position& p, const std::optional<cpr::Position>& anchor) {
    return !(anchor && cpr::distanceNm(p, *anchor) > limits::kRangeNm);
}

struct DecoderConfig {
    std::optional<cpr::Position> reference;  // the input's reference position (spec section 8.1), may be absent
    std::size_t max_keys = 20000;            // our choice (design section 3)
    std::chrono::seconds idle_evict{300};    // our choice (design section 3)
};

struct DecodeCounters {
    uint64_t frames = 0, crc_fail = 0, crc_fixed = 0, ap_unconfirmed = 0, no_position = 0, cpr_pending = 0,
             cpr_rejected = 0, decoded = 0, unmapped = 0, unknown_type = 0, evicted = 0, observations = 0;
    // Comm-B register inference (DF20/21) and ACAS RAs (spec section 14 item 8).
    uint64_t commb_bds40 = 0, commb_bds50 = 0, commb_bds60 = 0, commb_ambiguous = 0, commb_other = 0,
             commb_empty = 0, commb_unknown = 0, acas_ra = 0, acas_ra_tc28 = 0;
    uint64_t rs_errors = 0;  // UAT only: Reed-Solomon symbol errors corrected, summed over the frames (the "rs=" values)
};

}  // namespace skycued::decode
