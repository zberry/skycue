#pragma once

// StatsStore: the statistics view's aggregation inside skycued (spec
// docs/superpowers/specs/2026-11-03-skycue-stats-view-design.md §4). The
// Engine feeds it on its own thread; no I/O, no locking. Every time is the
// data clock in whole seconds (floored), so a replay builds a sensible history.
//
// Feeding (Engine): observe(obs, state, result) once per observation, after
// table.apply and before the send/drop decision, so a sent()/dropped() for an
// aircraft always finds its ledger entry; sent() per update sent, dropped()
// per drop sent (with the filter decision for filtered); tick() about once a
// second with the EngineStats.
//
// Time series. Two rings of contiguous buckets, minute (60 s, the last 1440)
// and hour (3600 s, the last 720), both updated directly by every call.
// Placing a time t into a ring:
//   - empty ring: one bucket for t;
//   - t inside the newest bucket or newer: advance; every intermediate empty
//     bucket is appended, the oldest popped past the cap; a gap of a whole
//     ring or more empties the ring first and keeps only t's bucket;
//   - t in the bucket just before the newest: that bucket (made, at the front,
//     when only the newest exists, and series_since moves back to t);
//   - anything older: observe() restarts the series (both rings emptied,
//     seriesSinceS() = t, one INFO "stats: the data clock moved backwards to
//     <iso>; the series restart", at most once a minute); sent(), dropped()
//     and tick() never restart: an older time goes into the newest bucket.
// Restarts leave the histograms, coverage, grid and ledger alone.
//
// A bucket's `aircraft` counts distinct TrackIds (address and type) through
// an address set for the newest and for the previous bucket of each ring;
// older sets are discarded, so at most two minutes' and two hours' ids are
// held (addressSetEntries()). `new_aircraft` counts ids not yet in the
// ledger (an evicted aircraft heard again is new again).
//
// observe(): bucket observations, by_input[input id or "unknown"] (at most
// kStatsInputsMax ids, the rest under "other"; counts(input) the same),
// positions (the observation has a finite lat/lon on the globe, |lat| <= 90
// and |lon| <= 180; anything else counts as no position everywhere), emergencies (state.latest's
// emergency set and not "none", or squawk 7500/7600/7700), max_range_nm and
// its id. The numeric histograms use the observation's own fields: altitude
// = alt_baro_ft else alt_geom_ft, 0 on the ground; speed gs_kt; vertical
// rate baro_rate_fpm else geom_rate_fpm; rssi. Range, elevation (finite
// result values), coverage, rssi_by_range and the bucket/ledger maximum range
// need a position and a receiver; the grid needs a position only (0.05°
// cells, new cells ignored once grid_cells_max exist). Coverage: sector =
// floor(bearing / 5) mod 72 of geo::bearingDeg from the receiver, band =
// altitudeBand of the observation's altitude; a cell keeps its maximum range
// with the id and time of that maximum and its observation count. Identity
// fields (callsign, squawk, registration, category, emergency) come from
// state.latest. Categorical counts: category ([A-D][0-7], else "none"),
// source and addr_type (recording::sourceName/addressTypeName), input,
// decision (decisionName), squawk_emergency ("7500", "7600", "7700").
// An observation with source uat_978 sets uatSeen().
//
// Ledger (per TrackId), made by the first observation: first/last seen,
// observations, positions, sessions (from 1, +1 when an observation follows
// the previous one by more than kStatsSessionGapS), sent_updates, drops (any
// reason), emergencies, the maximum range with its time, position and
// bearing, min/max altitude, max ground speed, min/max rssi, the last
// kStatsListMax distinct callsigns, squawks and inputs (newest last, a repeat
// moves to the end), registration, category, source. When an insert makes it
// exceed aircraft_max, the max(1, aircraft_max / 20) entries with the oldest
// last_seen_s, never the aircraft being observed, are erased and counted in
// evicted().
//
// sent() / dropped(): the bucket counters (drops by reason and, filtered, by
// decision, through countDropIn as Engine::countDrop) and the ledger entry
// when there is one. tick(): adds the positive deltas of uat_downlink,
// uat_uplink and fisb_products since the previous tick (the first tick and a
// counter that went down only set the base); a positive delta sets uatSeen().
// It sets tcp_clients/web_clients of the newest minute to the sample and of
// the newest hour to the maximum. On an empty series a tick stores only a
// positive delta (which starts the series).
//
// since: when accumulation began (the first stored value on an empty store,
// or reset()); a later, older observation does not move it back (only
// series_since follows a late bucket made before the first one). dirty(): set by any stored change (observe, sent, dropped,
// reset; a tick only when a value changes, never for placing empty buckets),
// cleared by markSaved().

#include "filters.hpp"
#include "observation.hpp"
#include "track_table.hpp"

#include <skycue/types.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace skycued {

struct EngineStats;

constexpr std::size_t kStatsMinuteBuckets = 1440;  // 24 h
constexpr std::size_t kStatsHourBuckets = 720;     // 30 days
constexpr std::size_t kStatsInputsMax = 16;        // by_input ids before "other"
constexpr std::size_t kStatsListMax = 4;           // ledger callsigns, squawks, inputs
constexpr double kStatsGridCellDeg = 0.05;
constexpr int64_t kStatsSessionGapS = 600;
constexpr int kStatsSectors = 72;  // 5° each
constexpr int kStatsBands = 8;     // altitudeBand
constexpr int kStatsRssiRangeBins = 50;  // 10 nm each
constexpr int kStatsRssiBins = 25;       // 2 dB each, from -50

enum class StatsRes { minute, hour };
enum class StatsHist { range, altitude, speed, vertical_rate, elevation, rssi };
enum class StatsCount { category, source, addr_type, input, decision, squawk_emergency };

struct StatsLimits {
    std::size_t aircraft_max = 20000;
    std::size_t grid_cells_max = 40000;
};

struct StatsBucket {
    int64_t start_s = 0;  // the bucket's first second
    uint64_t observations = 0, positions = 0, aircraft = 0, new_aircraft = 0, sent_updates = 0;
    uint64_t drops_timeout = 0, drops_filtered = 0, drops_source_lost = 0;
    uint64_t drops_blocked = 0, drops_out_of_range = 0, drops_below_elevation = 0, drops_no_position = 0;
    uint64_t uat_downlink = 0, uat_uplink = 0, fisb_products = 0, emergencies = 0;
    uint64_t tcp_clients = 0, web_clients = 0;  // minute: the last sample; hour: the maximum
    double max_range_nm = 0;
    std::optional<skycue::TrackId> max_range_id;
    std::map<std::string, uint64_t> by_input;
};

// Bin i counts values in [start + i*bin, start + (i+1)*bin); below the first
// edge goes to bin 0, at or above the last to the last bin.
struct StatsHistogram {
    double start = 0, bin = 1;
    std::vector<uint64_t> counts;
};

struct CoverageCell {
    double max_range_nm = 0;
    std::optional<skycue::TrackId> id;  // the holder of max_range_nm
    int64_t time_s = 0;                 // when it was heard there
    uint64_t n = 0;                     // observations in the cell
};

// The cell [lat_i * 0.05, (lat_i + 1) * 0.05) x [lon_i * 0.05, ...), lat_i = floor(lat / 0.05).
struct GridCell {
    int32_t lat_i = 0, lon_i = 0;
    uint64_t n = 0;
};

struct AircraftStats {
    skycue::TrackId id;
    int64_t first_seen_s = 0, last_seen_s = 0;
    uint64_t observations = 0, positions = 0, sessions = 0, sent_updates = 0, drops = 0, emergencies = 0;
    std::optional<double> max_range_nm;
    int64_t max_range_time_s = 0;
    double max_range_lat = 0, max_range_lon = 0, max_range_bearing_deg = 0;
    std::optional<int32_t> min_alt_ft, max_alt_ft;
    std::optional<double> max_gs_kt, min_rssi, max_rssi;
    std::vector<std::string> callsigns, squawks, inputs;  // distinct, newest last, at most kStatsListMax
    std::optional<std::string> registration, category;
    skycue::Source source = skycue::Source::other;  // of the newest observation
};

struct SeriesTotals {
    uint64_t observations = 0, positions = 0, new_aircraft = 0, sent_updates = 0;
    uint64_t drops = 0;  // timeout + filtered + source_lost
    uint64_t emergencies = 0;
    double max_range_nm = 0;
    std::optional<skycue::TrackId> max_range_id;
};

// floor(bearing / 5) mod 72.
int coverageSector(double bearing_deg);
// 0 on the ground; 7 without an altitude; else by the upper edges 2500,
// 5000, 10000, 20000, 30000, 40000 ft (lower edge inclusive), 6 above.
int altitudeBand(std::optional<int32_t> alt_ft, bool on_ground);
// The bin of a value (clamped to the first and last bin); v must not be NaN.
std::size_t histBin(StatsHist h, double v);
std::size_t histSize(StatsHist h);
// "0-2.5k", "2.5-5k", "5-10k", "10-20k", "20-30k", "30-40k", "40k+", "unknown".
const char* bandName(int band);

class StatsStore {
public:
    using Time = std::chrono::system_clock::time_point;

    explicit StatsStore(std::optional<ReceiverPosition> receiver, StatsLimits limits = StatsLimits());

    void observe(const TrackObservation& obs, const TrackState& state, const FilterResult& result);
    void sent(skycue::TrackId id, Time now);
    void dropped(skycue::TrackId id, skycue::DropReason reason, std::optional<Decision> decision, Time now);
    void tick(Time now, const EngineStats& s);
    // Empties everything (series, histograms, coverage, grid, counts, ledger,
    // evicted, uatSeen); since and series_since become now.
    void reset(Time now);

    // Oldest first, contiguous in time.
    const std::deque<StatsBucket>& series(StatsRes r) const;
    // Over the buckets whose start_s is in [from_s, to_s].
    SeriesTotals totals(StatsRes r, int64_t from_s, int64_t to_s) const;

    const StatsHistogram& histogram(StatsHist h) const;
    // sector 0..kStatsSectors-1, band 0..kStatsBands-1 (std::out_of_range otherwise).
    const CoverageCell& coverage(int sector, int band) const;
    // range_bin 0..kStatsRssiRangeBins-1, rssi_bin 0..kStatsRssiBins-1 (0 outside).
    uint64_t rssiByRange(int range_bin, int rssi_bin) const;
    std::vector<GridCell> gridCells() const;  // sorted by lat_i, then lon_i
    bool gridFull() const;
    const std::map<std::string, uint64_t>& counts(StatsCount c) const;

    const AircraftStats* aircraft(skycue::TrackId id) const;  // nullptr when not in the ledger
    std::size_t aircraftCount() const { return ledger_.size(); }
    uint64_t evicted() const { return evicted_; }
    template <class F> void forEachAircraft(F f) const
    {
        for (const auto& kv : ledger_)
            f(kv.second);
    }

    std::optional<int64_t> sinceS() const { return since_s_; }
    std::optional<int64_t> seriesSinceS() const { return series_since_s_; }
    bool uatSeen() const { return uat_seen_; }
    std::optional<ReceiverPosition> receiver() const { return receiver_; }
    const StatsLimits& limits() const { return limits_; }

    bool dirty() const { return dirty_; }
    void markSaved() { dirty_ = false; }
    // Entries in the open distinct-address sets (both rings, both sets).
    std::size_t addressSetEntries() const;

private:
    struct Ring {
        int64_t len = 60;
        std::size_t cap = 0;
        std::deque<StatsBucket> buckets;
        std::unordered_set<skycue::TrackId> ids_newest, ids_prev;
    };
    // Where a time went in one ring: the bucket and its address set.
    struct Slot {
        StatsBucket* bucket = nullptr;
        std::unordered_set<skycue::TrackId>* ids = nullptr;
    };
    struct Placed {
        Slot minute, hour;
    };
    enum class Older { restart, clamp };

    // The one placement of a timed call (see the header comment).
    Placed place(int64_t t_s, Older older);
    static Slot placeIn(Ring& ring, int64_t t_s);
    void restartSeries(int64_t t_s);
    void countInput(std::map<std::string, uint64_t>& m, const std::string& key);
    void evictOldest(skycue::TrackId keep);  // never evicts `keep`
    void clearAll();

    std::optional<ReceiverPosition> receiver_;
    StatsLimits limits_;
    Ring minute_, hour_;
    std::array<StatsHistogram, 6> hist_;
    std::array<CoverageCell, kStatsSectors * kStatsBands> coverage_{};
    std::vector<uint64_t> rssi_by_range_;  // kStatsRssiRangeBins x kStatsRssiBins, range major
    std::unordered_map<int64_t, uint64_t> grid_;  // (int64_t(lat_i) << 32) | uint32_t(lon_i)
    std::array<std::map<std::string, uint64_t>, 6> counts_;
    std::unordered_map<skycue::TrackId, AircraftStats> ledger_;
    uint64_t evicted_ = 0;
    std::optional<int64_t> since_s_, series_since_s_;
    bool uat_seen_ = false;
    bool dirty_ = false;
    // tick(): the previous counters (unset before the first tick).
    std::optional<uint64_t> base_downlink_, base_uplink_, base_products_;
    std::optional<std::chrono::steady_clock::time_point> last_restart_log_;
};

}  // namespace skycued
