#pragma once

// Product 413, generic text (docs/fisb-reference.md §3, §4): DLAC text
// holding METAR/SPECI, TAF/TAF.AMD/TAF COR, PIREP and WINDS reports.

#include "fisb/apdu.hpp"
#include "fisb/products.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skycued {
namespace fisb {

// Splits the payload into reports on RS (each ends LF RS in the data, not
// "\n\x03" as reference §4 says), drops empty ones, and classifies each by
// its first word(s): METAR/SPECI -> metar, TAF[.AMD|.COR| AMD| COR] -> taf,
// PIREP/UA/UUA -> pirep, WINDS -> winds, anything else -> other. station is
// the first 3-4 character letters-and-digits token after the type words.
// issued is the first ddhhmmZ group of the first line resolved against ref,
// else the APDU time. Winds get period_h from the APDU hour and the valid
// time (fisb-decode's WIND_MATRIX); a combination outside the matrix leaves
// it empty. Never throws.
std::vector<TextRecord> parseTextProduct(const ApduHeader& h, const uint8_t* p, std::size_t n, TimePoint ref);

// The WIND_MATRIX lookup alone: APDU (product available) hour, valid hhmm.
std::optional<uint8_t> windsPeriod(unsigned apdu_hour, unsigned valid_hhmm);

}  // namespace fisb
}  // namespace skycued
