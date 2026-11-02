#pragma once

// Mode S parity: the 24-bit CRC with generator polynomial
// x^24 + 0xFFF409 (0x1FFF409), over a whole 56- or 112-bit frame.

#include <cstdint>
#include <vector>

namespace iqgen {

// The remainder of the frame's bits (all of them, parity field included)
// divided by the generator: 0 for a correct DF11 with IID 0 or DF17/18; the
// interrogator ID for a DF11 answering one; the address for an AP field.
uint32_t modesCrcResidual(const std::vector<uint8_t>& frame);

// The downlink format (first 5 bits); -1 for an empty frame.
int modesDf(const std::vector<uint8_t>& frame);

// The frames readsb is expected to forward whatever it already knows:
// DF11 (7 bytes) or DF17/18 (14 bytes) with a residual of exactly 0.
bool modesClean(const std::vector<uint8_t>& frame);

}  // namespace iqgen
