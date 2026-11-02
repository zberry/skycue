#pragma once

// The analysis behind skycue-stats: summarises the records of one or more
// recordings (received stream, sent stream, readsb JSON captures; see
// recording_reader.hpp) into a StatsReport, and renders it as text or JSON.
// docs/operating.md shows the output.
//
// "Observations" below are the received stream's "obs" records (readsb lines
// included); when the input has none, the sent stream's update/keyframe
// records stand in for them (StatsReport::basis says which). Sections 2-7
// and 9 are computed from them; the overview, the gaps and the sent-stream
// section from every record.

#include "recording_reader.hpp"

#include <skycue/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skycued {

// One input file as its reader saw it.
struct StatsInput {
    std::string name;  // as given on the command line
    std::string kind;  // "received", "sent", "readsb-raw", several joined with "+", or "empty"
    uint64_t records = 0, skipped = 0;
};

struct StatsOptions {
    std::optional<double> rx_lat, rx_lon;  // both or neither; neither: range/elevation/azimuth are skipped
    double rx_alt_m = 0;                   // receiver altitude plus antenna height (elevation)
    std::string receiver_from;             // "--lat/--lon" or "--config" (for the report)
    std::size_t top_n = 15;
    double range_bin_nm = 10;
    int alt_bin_ft = 5000;
    double gap_s = 5;
    std::vector<StatsInput> inputs;
};

struct StatsReport {
    using Time = std::chrono::system_clock::time_point;

    struct Bin {
        double start = 0, end = 0;
        uint64_t obs = 0, aircraft = 0;  // aircraft: distinct TrackIds in the bin
    };
    struct Extreme {  // the observation with the largest value
        skycue::TrackId id;
        Time time{};
        double value = 0;
    };

    StatsOptions options;
    std::string basis;  // "received", "sent", or "" (no observations)

    struct Overview {
        std::vector<std::string> stream_kinds;  // "received", "sent", "readsb-raw", in that order
        std::optional<Time> first, last;
        double duration_s = 0;
        uint64_t records = 0;
        std::map<std::string, uint64_t> by_kind;  // recordKindName -> count (kinds present only)
        double mean_per_s = 0;                    // records / duration (records when the duration is 0)
        uint64_t peak_per_s = 0;                  // most records in one UTC second
    } overview;

    struct Count {
        uint64_t aircraft = 0, obs = 0;
    };
    struct SquawkHit {
        skycue::TrackId id;
        std::string squawk;
        uint64_t obs = 0;
        Time first{}, last{};
    };
    struct Aircraft {
        uint64_t distinct = 0, observations = 0, on_ground = 0, airborne = 0;
        std::map<std::string, Count> by_source, by_addr_type;  // by name
        uint64_t with_callsign = 0, with_registration = 0;     // aircraft
        std::vector<SquawkHit> emergency_squawks;              // 7500/7600/7700, by first occurrence
    } aircraft;

    struct Range {
        bool computed = false;  // false: receiver position unknown
        uint64_t positions = 0;
        std::vector<Bin> bins;  // from 0 nm up to the bin of the farthest observation
        std::optional<Extreme> max;
        double median_nm = 0;  // 0 when there are no positions
    } range;

    struct Altitude {  // baro, else geometric
        uint64_t with_altitude = 0;
        std::vector<Bin> bins;  // from the lowest to the highest bin seen
        std::optional<Extreme> max;
    } altitude;

    struct Elevation {
        bool computed = false;
        uint64_t samples = 0;     // observations with position and altitude
        uint64_t below_zero = 0;  // not in any bin
        std::vector<Bin> bins;    // 18 bins of 5 deg, 0-90 (90 itself in the last)
    } elevation;

    struct Sector {
        double start = 0, end = 0;
        uint64_t obs = 0, aircraft = 0;
        std::optional<double> max_range_nm;
    };
    struct Azimuth {
        bool computed = false;
        std::vector<Sector> sectors;  // 12 of 30 deg, by initial great-circle bearing from the receiver
    } azimuth;

    struct Speed {
        uint64_t gs_samples = 0;
        double gs_min = 0, gs_median = 0, gs_max = 0;
        uint64_t vr_samples = 0, climbing = 0, descending = 0, level = 0;  // level: |rate| <= 100 ft/min
    } speed;

    struct Gap {
        Time start{}, end{};
        double length_s = 0;
    };
    std::vector<Gap> gaps;  // between consecutive records, longer than options.gap_s

    struct AircraftRow {
        skycue::TrackId id;
        std::string source;  // the source of most of its observations
        std::optional<std::string> callsign, registration, category;  // the last seen
        uint64_t obs = 0;
        Time first{}, last{};
        std::optional<int32_t> min_alt_ft, max_alt_ft;
        std::optional<double> max_range_nm, mean_gs_kt;
    };
    std::vector<AircraftRow> top;  // by observations (then id), at most options.top_n

    struct Sent {
        bool present = false;  // any update/keyframe/drop/filter_change record
        uint64_t updates = 0, keyframes = 0, keyframe_flagged = 0;
        std::map<std::string, uint64_t> drops;  // every DropReason name, 0 included
        std::vector<std::pair<Time, std::string>> filter_changes;
        std::vector<std::pair<skycue::TrackId, uint64_t>> updates_per_aircraft;  // top N
        bool compared = false;  // both received observations and updates present
        uint64_t received_obs = 0;
        double compression = 0;                // received_obs / updates
        std::vector<skycue::TrackId> never_sent;  // received aircraft without an update/keyframe
    } sent;
};

// Records need not be sorted (they are ordered by time, stably, first).
StatsReport computeStats(const std::vector<RecordingRecord>& records, const StatsOptions& options);

// Deterministic, locale-independent renderings of the whole report.
std::string formatStatsText(const StatsReport& report);
std::string formatStatsJson(const StatsReport& report);  // one JSON object, no trailing newline

// "a1b2c3"; non-ICAO address types get readsb's leading '~'.
std::string trackIdText(const skycue::TrackId& id);

}  // namespace skycued
