#pragma once

// DLAC 6-bit text (docs/fisb-reference.md §4): 4 characters per 3 bytes,
// MSB first.

#include <cstddef>
#include <cstdint>
#include <string>

namespace skycued {
namespace fisb {

// Decodes codes until ETX (0) or until fewer than 6 bits remain, so padding
// bits never become a character. Codes 1-26 are A-Z, 30 is '\n', 29 (record
// separator) is kept as '\x1e' so 413 text can be split on it, 27 (null
// character) is dropped, 31 is '|', 32-63 are " !\"#$%&'()*+,-./0-9:;<=>?".
// 28 is TAB: the next code (0-63, possibly in the next 3-byte group) is a
// count of spaces, not a character; a TAB with no count emits nothing.
std::string decodeDlac(const uint8_t* p, std::size_t n);

}  // namespace fisb
}  // namespace skycued
