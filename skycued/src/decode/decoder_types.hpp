#pragma once

#include <chrono>
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
