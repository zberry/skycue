#pragma once

// Hex text and bit vectors for the IQ generators. A bit vector holds one
// transmitted bit per element (0 or 1), in transmission order.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace iqgen {

// Even length, [0-9a-fA-F] only (no prefix, no separators). On success out
// holds the bytes; on failure it is left unchanged.
bool parseHex(const std::string& hex, std::vector<uint8_t>& out);

std::string toHex(const std::vector<uint8_t>& bytes, bool upper);

// The low nbits of value, MSB first. Throws std::invalid_argument when nbits > 64.
void appendBits(std::vector<uint8_t>& bits, uint64_t value, unsigned nbits);

// Each of the n bytes at p, MSB first.
void appendBytes(std::vector<uint8_t>& bits, const uint8_t* p, std::size_t n);

}  // namespace iqgen
