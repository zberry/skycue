#pragma once

// CAT021 encoder, the inverse of decodeBlock. It exists for tests and test
// tools (the public sample data is GPL, reference §7); skycued never sends
// CAT021.

#include "asterix/cat021.hpp"

#include <cstdint>
#include <vector>

namespace skycued {
namespace asterix {

// Writes the FSPEC (with FX bits) and the items in FRN order, from the modelled
// fields and from `opaque` for the rest. A modelled field wins over an opaque
// entry for the same FRN, except RE (FRN 48): opaque[48] is written when it is
// present (it may carry subfields besides SGV), and an SGV-only RE is built from
// `sgv` only when opaque[48] is absent. `bytes` is ignored. FX bits inside
// 040/090 `more` are recomputed. Ident characters outside A-Z, 0-9 and space
// are written as space; a shorter ident is space-padded.
// Throws std::invalid_argument for a value that does not fit its field
// (address > 0xFFFFFF, mode3a > 07777, ident longer than 8 characters, ...),
// an opaque item whose bytes do not measure to their own length or whose FRN
// is spare or out of range, a half-set pair (sac/sic, lat/lon), or a record
// with no item.
std::vector<uint8_t> encodeRecord(const Cat021Record& r);

// CAT 0x15, big-endian LEN, then the records.
std::vector<uint8_t> encodeBlock(const std::vector<Cat021Record>& records);

}  // namespace asterix
}  // namespace skycued
