#pragma once

// Text With Graphic Overlay payloads, products 8 and 11-17
// (docs/fisb-reference.md §6). One APDU (after reassembly) holds either text
// records (format 2) or graphic overlay records (format 8); the two halves of
// a report arrive in separate APDUs with the same key and are paired by the
// store, not here.

#include "fisb/apdu.hpp"
#include "fisb/products.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skycued {
namespace fisb {

// pid-year-report-location-month (reference §6.5). location is the raw
// 24-bit DLAC field of the TWGO header (0 when absent), so it needs 32 bits;
// month is the APDU month (0 when the APDU has none). year is the record's
// raw 7-bit report year (two digits, or one for some NOTAMs).
struct TwgoKey {
    uint16_t pid = 0;
    uint8_t year = 0;
    uint32_t report = 0;
    uint32_t location = 0;
    uint8_t month = 0;
    bool operator<(const TwgoKey& o) const;
    bool operator==(const TwgoKey& o) const;
    std::string str() const;  // e.g. "8-15-12045-KSCK-1"; an empty location stays empty
};

// Per-record bookkeeping, for diagnostics and the exact-length test.
struct TwgoRecordInfo {
    uint8_t format = 0;     // 2 text, 8 graphic
    std::size_t declared = 0;  // the record's length field, bytes
    std::size_t consumed = 0;  // bytes the fields actually used
    uint8_t geometry = 0;   // graphic only: the 4-bit geometry type
    uint8_t vertices = 0;   // graphic only
    bool ignored = false;   // graphic record skipped (param flag, bad time format,
                            // unsupported operator or geometry)
};

struct TwgoParsed {
    TwgoKey key;
    std::optional<std::string> text;  // format 2: the text (may be empty: a TFR keep-alive)
    std::vector<Shape> shapes;        // format 8
    bool has_graphics = false;        // format 8
    bool cancel = false;              // text status 0, or a graphic object_status 13
    uint8_t object_status = 0;        // first graphic record's (15 in effect, 13 cancelled)
    std::optional<TimePoint> start, end;  // graphic applicability window
    std::string label;                // DLAC object label (e.g. "KSCK"), empty when numeric
    std::string location;             // TWGO header location, decoded
    uint8_t record_format = 0;
    std::vector<TwgoRecordInfo> records;
    std::size_t leftover = 0;  // payload bytes after the declared records
};

// p/n is the (reassembled) APDU payload starting with the 6-byte TWGO
// header. ref resolves the applicability dates (reference §7). nullopt when
// the header is short, the format is neither 2 nor 8, the record count is 0,
// or a record's length is impossible or runs past the payload. Multi-record
// shapes (64-vertex continuations) are merged, two-altitude polygon rings
// become one polygon with low/high altitudes, and multi-vertex points and
// circles become one shape per vertex (reference §6.4). Never throws.
std::optional<TwgoParsed> parseTwgo(const ApduHeader& h, const uint8_t* p, std::size_t n, TimePoint ref);

// The end of validity in a NOTAM's text: the second half of its first
// "yymmddhhmm-yymmddhhmm" group (an "EST" suffix is ignored; years are
// 20yy), as fisb-decode's NOTAM_TIMES_RE reads it. nullopt for "-PERM", no
// group, or an impossible date.
std::optional<TimePoint> notamTextEnd(const std::string& text);

}  // namespace fisb
}  // namespace skycued
