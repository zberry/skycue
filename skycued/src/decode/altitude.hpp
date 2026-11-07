#pragma once

// Altitude and identity code fields of Mode S frames: the 13-bit AC field
// (DF0/4/16/20), the 12-bit ES altitude (TC 9-18) and the 13-bit ID field
// (DF5/21 and TC28). Pure functions on the field value.

#include <cstdint>
#include <optional>
#include <string>

namespace skycued::decode {

// AC13 as transmitted (bit 12 of the value is the first bit on the wire).
// 0, the M bit (metric altitude) and out-of-range values give nullopt; Q=1 is
// 25 ft steps from -1000 ft; Q=0 is the Gillham (Mode C) code in 100 ft steps.
std::optional<int32_t> altitudeAc13(uint32_t ac13);

// AC12 is AC13 without the M bit (Q is bit 8 from the left). 0 gives nullopt.
std::optional<int32_t> altitudeAc12(uint32_t ac12);

// ID13 as transmitted -> four octal digits "ABCD" (the Mode A code).
std::optional<std::string> squawkId13(uint32_t id13);

}  // namespace skycued::decode
