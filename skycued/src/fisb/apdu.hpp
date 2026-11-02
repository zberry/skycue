#pragma once

// FIS-B APDU header (docs/fisb-reference.md §2): flags, 11-bit product ID,
// segmentation flag, time option and time fields, and for segmented APDUs
// the product file ID, segment count and 1-based segment number. The payload
// starts at the next byte boundary (header_bytes).

#include <cstddef>
#include <cstdint>
#include <optional>

namespace skycued {
namespace fisb {

struct ApduHeader {
    bool a_flag = false, g_flag = false, p_flag = false, s_flag = false;
    uint16_t product_id = 0;  // 11 bits
    uint8_t t_opt = 0;        // 0 hh:mm, 1 hh:mm:ss, 2 mm/dd hh:mm, 3 mm/dd hh:mm:ss
    uint8_t hours = 0, minutes = 0;
    std::optional<uint8_t> month, day;  // T-opt 2 and 3
    std::optional<uint8_t> seconds;     // T-opt 1 and 3 (experimental, see parseApdu)
    bool segmented = false;             // == s_flag
    // Segmented only (0 otherwise). The wire fields are 10, 9 and 9 bits wide,
    // hence 16-bit storage for all three.
    uint16_t apdu_number = 0;     // product file ID: with product_id, the reassembly key
    uint16_t segment_number = 0;  // the wire's "APDU number", 1-based
    uint16_t segment_count = 0;   // the wire's "product file length"
    std::size_t header_bytes = 0;  // payload offset: 4, 5, 7 or 9 for T-opt 0/2
};

enum class ApduStatus {
    ok,
    truncated,                 // fewer bytes than the header needs
    experimental_time_option,  // T-opt 1 or 3 with experimental_time_opts off
};

// Time options 0 and 2 are always decoded. 1 and 3 (the open decoders
// disagree on their layout, reference §2 [U]) are decoded the MUT/STX way
// (seconds present, 6 bits after the minutes) only when
// experimental_time_opts; otherwise this returns nullopt with status
// experimental_time_option so the caller can count it. Never reads past
// p + n; never throws.
std::optional<ApduHeader> parseApdu(const uint8_t* p, std::size_t n, bool experimental_time_opts,
                                    ApduStatus* status = nullptr);

}  // namespace fisb
}  // namespace skycued
