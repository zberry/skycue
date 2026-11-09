#pragma once

// Comm-B (DF20/21 MB field) register decoding for BDS 4,0 / 5,0 / 6,0 and the
// inference of which register a reply holds. Pure functions on the 56-bit MB,
// passed as a uint64_t with the amendment's bit 1 as the most significant bit
// (bit 55); see field56.hpp. Layouts and validity rules: decoder design
// section 14, item 8.

#include <cstdint>
#include <optional>

namespace skycued::decode {

struct Bds40 {
    std::optional<int32_t> sel_alt_mcp_ft, sel_alt_fms_ft;
    std::optional<float> qnh_mb;
    bool modes_valid = false;
    bool vnav = false, althold = false, approach = false;
};
struct Bds50 {
    std::optional<float> roll_deg, track_deg, gs_kt, track_rate_dps, tas_kt;
};
struct Bds60 {
    std::optional<float> heading_mag_deg, mach;
    std::optional<uint16_t> ias_kt;
    std::optional<int32_t> baro_rate_fpm, inertial_rate_fpm;
};

// Each returns nullopt when a check fails: a status-0 field with a non-zero
// value, a non-zero reserved bit, or a value outside the amendment's range.
std::optional<Bds40> decodeBds40(uint64_t mb);
std::optional<Bds50> decodeBds50(uint64_t mb);
// With alt_ft, IAS and Mach (when both present) must agree with that altitude
// within 20 % (Mach estimated from IAS at ISA).
std::optional<Bds60> decodeBds60(uint64_t mb, std::optional<int32_t> alt_ft);

enum class CommbKind { bds40, bds50, bds60, ambiguous, other, empty, unknown };

struct CommbContext {
    std::optional<float> track_deg, gs_kt;
    std::optional<int32_t> baro_rate_fpm, alt_ft;
};
struct CommbResult {
    CommbKind kind = CommbKind::unknown;
    std::optional<Bds40> b40;
    std::optional<Bds50> b50;
    std::optional<Bds60> b60;
};

// all-zero -> empty; exactly one register passing -> that one; several -> the
// context breaks the tie (see commb.cpp) or the result is ambiguous with
// nothing set; none -> other (BDS 1,0 / 1,7 / 2,0 / 3,0 recognised) or unknown.
CommbResult inferCommb(uint64_t mb, const CommbContext& ctx);

}  // namespace skycued::decode
