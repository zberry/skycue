#pragma once

#include "track_table.hpp"

#include <skycue/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

namespace skycued {

// A curated list of aircraft (allow or block list).
//
// File grammar: one entry per line; '#' starts a comment at the start of a
// line or after whitespace; blank lines are ignored; surrounding whitespace is
// trimmed. An entry is either
//   - six hex digits, any case: an address (e.g. "a1b2c3"), or
//   - a registration, 2-10 of [A-Za-z0-9-] (e.g. "N123AB", "G-ABCD").
// Six hex digits are always read as an address, so a registration that is
// itself six hex digits (rare, some non-US) cannot be listed by registration.
// Anything else is rejected with std::runtime_error("<name>:<line>: bad entry '<text>'").
//
// A registration that is a valid US N-number is also stored as its ICAO
// address, so it matches a track that reports no registration.
class IdList {
public:
    // Reads the whole file and parses it; errors name the path (and line).
    static IdList loadFile(const std::string& path);
    // `name` is used in error messages.
    static IdList parse(const std::string& text, const std::string& name = "<list>");

    // True when the list holds `id.address` as hex (any address type), or an
    // N-number whose ICAO address is `id.address` (ICAO addresses only), or a
    // registration equal to `registration` (case-insensitive, trimmed).
    bool contains(skycue::TrackId id, const std::optional<std::string>& registration) const;
    // Number of entries (lines) parsed.
    size_t size() const { return entries_; }

private:
    std::unordered_set<uint32_t> hex_;            // listed addresses, any type
    std::unordered_set<uint32_t> n_number_icao_;  // addresses derived from N-numbers
    std::unordered_set<std::string> registrations_; // upper case
    size_t entries_ = 0;
};

struct ReceiverPosition {
    double lat, lon;  // degrees
    double alt_m;     // site altitude plus antenna height, metres
};

struct FilterConfig {
    double max_range_nm;       // a target further than this is out_of_range
    double min_elevation_deg;  // a target lower than this is below_elevation
};

enum class Decision { send, blocked, allow_listed, out_of_range, below_elevation, no_position };

struct FilterResult {
    Decision decision;
    double range_nm;       // NaN without a position
    double elevation_deg;  // NaN without a position
    bool alt_is_geom;      // the elevation used geometric altitude
};

inline bool isSend(Decision d) { return d == Decision::send || d == Decision::allow_listed; }

// Decides whether a track is forwarded. No I/O.
//
// Order of evaluation: block list -> allow list -> range -> elevation -> send.
// Range/elevation are computed whenever the track has a position, including
// for blocked and allow-listed tracks (the UI shows them). A track without a
// position that is on neither list is no_position.
//
// Target altitude for the elevation: geometric if reported (alt_is_geom),
// else barometric; a target on the ground, or an airborne target with no
// altitude at all, is taken to be at the receiver altitude and still evaluated.
class FilterSet {
public:
    FilterSet(ReceiverPosition rx, FilterConfig config, IdList allow, IdList block);

    FilterResult evaluate(const TrackState& t) const;

    void setLists(IdList allow, IdList block);
    void setConfig(FilterConfig config);

private:
    ReceiverPosition rx_;
    FilterConfig config_;
    IdList allow_, block_;
};

} // namespace skycued
