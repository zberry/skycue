#pragma once

// Helpers for the 56-bit ME / MB / MV fields of Mode S frames, carried as a
// uint64_t with the field's first bit as the most significant (bit 55).
// Bit numbers here are 1-based within the field, as the standards and the
// decoder design (section 14, item 8) count them.

#include <cstdint>

namespace skycued::decode {

// n bits (n <= 32) starting at 1-based bit `first` of a 56-bit field.
inline uint32_t field56(uint64_t v, unsigned first, unsigned n)
{
    const unsigned shift = 56u - (first - 1u) - n;
    return static_cast<uint32_t>((v >> shift) & ((n >= 32 ? 0xFFFFFFFFull : ((1ull << n) - 1ull))));
}

// The same, read as an n-bit two's complement number (the sign bit is the
// field's first bit).
inline int32_t sfield56(uint64_t v, unsigned first, unsigned n)
{
    const uint32_t u = field56(v, first, n);
    const uint32_t sign = 1u << (n - 1u);
    return (u & sign) ? static_cast<int32_t>(u) - static_cast<int32_t>(sign << 1) : static_cast<int32_t>(u);
}

}  // namespace skycued::decode
