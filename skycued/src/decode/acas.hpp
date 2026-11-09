#pragma once

// ACAS resolution advisories: DF16 with VDS 0x30 (MV field) and TC28 subtype 2
// (ME field) carry the same body; DF0/DF16 common fields. Pure frame-level
// decoders (decoder design section 14, item 8).

#include <cstdint>
#include <optional>
#include <string>

#include "decode/frame.hpp"

namespace skycued::decode {

struct AcasRa {
    uint16_t ara = 0;  // 14 bits, bit 1 of the field is the most significant (0x2000)
    uint8_t rac = 0;   // 4 bits: do not pass below, do not pass above, do not turn left, do not turn right
    bool rat = false, mte = false;
    uint8_t tti = 0;
    std::optional<uint32_t> threat_address;  // tti 1
    std::optional<int32_t> threat_alt_ft;    // tti 2
    std::optional<float> threat_range_nm, threat_bearing_deg;
};

std::optional<AcasRa> decodeAcasRaMv(const Frame& f);    // DF16 with VDS 0x30; nullopt otherwise
std::optional<AcasRa> decodeAcasRaTc28(const Frame& f);  // TC28 subtype 2
std::string acasRaText(const AcasRa& ra);

struct AcasCommon {
    bool vs_ground = false;
    uint8_t sl = 0, ri = 0;
};
AcasCommon decodeAcasCommon(const Frame& f);  // DF0/DF16 VS, SL, RI (zeros for other frames)

}  // namespace skycued::decode
