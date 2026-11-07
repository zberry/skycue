#pragma once

// Adapters from the Beast frame splitter and the AVR line parser to the
// decoder's input (frame decoder design, 5.1 and 8.1).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "raw_formats.hpp"

namespace skycued::decode {

struct RawFrame {
    std::vector<uint8_t> bytes;    // 7 or 14 bytes
    std::optional<uint8_t> signal; // Beast signal byte; none for AVR
};

struct FrameSourceCounters {
    uint64_t modeac_skipped = 0, mlat_skipped = 0, other_skipped = 0, bad_avr = 0;
};

// Types 2 and 3 pass, type 5 as 3; type 1 counts modeac_skipped; type 4, 0xe3
// and any other type count other_skipped. A frame whose timestamp is the MLAT
// marker 0xFF004D4C4154 (a constant of the public Mode-S Beast data format
// description) counts mlat_skipped and is dropped.
std::optional<RawFrame> fromBeast(const beast::Frame& f, FrameSourceCounters& c);

// "*HEX;" and "@TSHEX;" lines; anything else counts bad_avr. The MLAT marker
// as an "@" timestamp is dropped and counted as for Beast.
std::optional<RawFrame> fromAvrLine(const std::string& line, FrameSourceCounters& c);

}  // namespace skycued::decode
