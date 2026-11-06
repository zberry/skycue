#pragma once

// The page's estimated class and address country of a track (www/app_pure.js:
// icaoCountry, icaoMilitary, callsignKind, trackClass, trackCountry), in C++
// so the daemon can count them over the stats ledger. Same rules and tables:
// the tables are icao_tables.inc, generated from app_pure.js by
// scripts/fixtures/gen-icao-tables.py (ctest icao_tables_current), and
// tests/test_icao_class.cpp runs the cases app_pure.test.js runs
// (tests/fixtures/icao_class_cases.json).
//
// A hex here is the page's: 6 hex digits for an ICAO address; anything else
// (readsb's "~a1b2c3" for a non-ICAO address, trackIdText) has no country and
// is not in a military block.

#include <skycue/types.hpp>

#include <optional>
#include <string>

namespace skycued {

enum class CallsignKind { none, airline, registration, military, other };

// The State the address is allocated to; nullopt when `hex` is not exactly 6
// hex digits (either case) or lies in no allocation block.
std::optional<std::string> icaoCountry(const std::string& hex);
// Whether `hex` (as icaoCountry) lies in a block States reserve for military
// aircraft: a heuristic list, not an ICAO rule.
bool icaoMilitary(const std::string& hex);
// What the callsign looks like, after trimming ASCII whitespace and
// upper-casing ASCII letters: military (a known military prefix and at least
// one more character), airline (3 letters + 1-4 letters/digits with a digit),
// registration (an N-number, or 4-6 letters), else other; none when empty.
CallsignKind callsignKind(const std::string& callsign);

// The address rules apply only to an ICAO address: icaoCountry(hex) when
// `addr_type` is icao, nullopt otherwise.
std::optional<std::string> trackCountry(const std::string& hex, skycue::AddressType addr_type);

struct ClassInput {
    std::string hex;                                     // as icaoCountry
    skycue::AddressType addr_type{skycue::AddressType::icao};
    std::string callsign;                                // "" when none
    std::string category;                                // emitter category, "A3"; "" when none
    std::string registration;                            // "" when none
};

// "military" (an ICAO address in a military block, or a military callsign),
// "airline" (an airline callsign, or any callsign with category A3-A5),
// "private" (category A1/A2/A7, a registration callsign, or a registration
// and no callsign), else "unknown". A static string.
const char* trackClass(const ClassInput& t);

}  // namespace skycued
