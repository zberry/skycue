#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace skycued {
namespace registration {

// US civil registrations (N-numbers) map one-to-one onto the ICAO address
// block 0xA00001..0xADF7C7 (915399 codes).
//
// An N-number is "N", 1-5 digits with no leading zero, then an optional
// suffix of letters from the 24-letter alphabet A-Z without I and O:
// 1-3 digits allow 0, 1 or 2 letters; 4 digits allow 0 or 1; 5 digits none.
//
// Addresses are assigned depth-first over the digit tree. The codes of one
// digit prefix with 1-3 digits are, in order: bare, A, AA..AZ, B, BA..BZ, ...,
// Z, ZA..ZZ (1 + 24 + 24*24 = 601); with 4 digits: bare, A..Z (25); with 5
// digits: bare (1). After a prefix's own codes come the subtrees of its ten
// children (prefix0 .. prefix9). Subtree sizes: 5 digits 1; 4 digits 35;
// 3 digits 951; 2 digits 10111; 1 digit 101711. N1 is 0xA00001, N2 0xA18D50.

// The N-number ("N" + upper case) for an address in the US block, else nullopt.
std::optional<std::string> nNumberFromIcao(uint32_t icao);

// The address for an N-number such as "N123AB"; case-insensitive, the
// leading N is optional. nullopt for anything that is not a valid N-number.
std::optional<uint32_t> icaoFromNNumber(const std::string& n);

} // namespace registration
} // namespace skycued
