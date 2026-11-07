#pragma once

// Mode S parity: the 24-bit CRC with generator polynomial
// x^24 + 0xFFF409 (0x1FFF409), over a whole 56- or 112-bit frame.

#include <cstdint>
#include <vector>

namespace skycued::coding {

// The remainder of the frame's bits (all of them, parity field included)
// divided by the generator: 0 for a correct DF11 with IID 0 or DF17/18; the
// interrogator ID for a DF11 answering one; the address for an AP field.
uint32_t modesCrcResidual(const std::vector<uint8_t>& frame);

// The downlink format (first 5 bits); -1 for an empty frame.
int modesDf(const std::vector<uint8_t>& frame);

// The frames readsb is expected to forward whatever it already knows:
// DF11 (7 bytes) or DF17/18 (14 bytes) with a residual of exactly 0.
bool modesClean(const std::vector<uint8_t>& frame);

// Single-bit correction of a 14-byte frame whose residual is non-zero: if the
// residual equals the syndrome of exactly one bit position outside the 5 DF
// bits, flip that bit and return true. Never corrects a 7-byte frame, a DF
// bit or two bits; the frame is left unchanged when it returns false.
bool syndromeFix1(std::vector<uint8_t>& frame);

}  // namespace skycued::coding
