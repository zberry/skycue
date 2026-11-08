#pragma once

// What the Engine exposes to an observer (the web server): its counters, a
// snapshot of every track, and the events it emits as things happen.

#include "filters.hpp"
#include "geom_altitude.hpp"
#include "fisb/fisb_store.hpp"
#include "inputs/input_types.hpp"

#include <skycue/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skycued {

// The population fit of GNSS-minus-baro altitude (AltitudeFit::fit) as of the data clock.
struct AltitudeFitStats {
    double a = 0, b = 0;            // diff ~= a + b * baro
    std::size_t samples = 0, aircraft = 0;
    double p90_ft = 0;              // 90th percentile of |residual|
    double age_s = 0;               // data clock minus the newest sample used
    bool linear = false;            // false: the median
    bool valid = false;             // false: no fit, the other fields are 0
};

struct EngineStats {
    uint64_t observations = 0, sent_updates = 0, sent_drops = 0, tracks_total = 0, tracks_forwarded = 0,
             uat_downlink = 0, uat_uplink = 0;
    bool readsb_connected = false, dump978_connected = false;
    uint64_t tcp_clients = 0, udp_targets = 0;  // Forwarder destinations now
    // Rebroadcast (TIS-B, ADS-R) positions ignored for a fresh direct one.
    uint64_t rebroadcast_suppressed = 0;
    // sent_drops by reason (the shutdown drops are counted in sent_drops only).
    uint64_t drops_timeout = 0, drops_filtered = 0, drops_source_lost = 0;
    // drops_filtered by the filter decision that dropped the track (they sum to it).
    uint64_t drops_blocked = 0, drops_out_of_range = 0, drops_below_elevation = 0, drops_no_position = 0;
    uint64_t recorder_dropped_received = 0, recorder_dropped_sent = 0;  // Recorder::droppedRecords()
    uint32_t web_clients = 0;  // as set by Engine::setWebClientCount
    // FIS-B (all 0 with FISB_ENABLE=no): uplinks fed to the decoder, products
    // it decoded, uplinks that were not valid hex or not 432 bytes (or whose
    // information frames overran), segment sets awaiting segments, and raster
    // blocks held across every mosaic.
    uint64_t fisb_uplinks = 0, fisb_products = 0, fisb_bad_frames = 0, fisb_segments_open = 0,
             fisb_mosaic_blocks = 0;
    // The receive time of the last uplink fed to the decoder (unset before any).
    std::optional<std::chrono::system_clock::time_point> fisb_last_uplink;
    // Uplinks dropped as the same hex from another input within 2 s
    // (UplinkDedupe): neither recorded nor decoded.
    uint64_t fisb_uplink_duplicates = 0;
    // Every input, built-ins first (InputManager::statsLines). readsb_connected
    // and dump978_connected above (and uat_downlink, uat_uplink) are those of
    // the built-ins readsb (or replay) and dump978, kept for one release.
    std::vector<InputStatsLine> inputs;
    AltitudeFitStats altitude_fit;
};

// Counts one drop in counters named as in EngineStats: drops_timeout,
// drops_filtered or drops_source_lost by reason (shutdown and stale have no
// counter of their own) and, for filtered, drops_blocked, drops_out_of_range,
// drops_below_elevation or drops_no_position by the decision that dropped it
// (send and allow_listed are never a drop). The one mapping
// Engine::countDrop and StatsStore::dropped share.
template <class Counters>
void countDropIn(Counters& c, skycue::DropReason reason, std::optional<Decision> decision)
{
    switch (reason) {
    case skycue::DropReason::timeout: ++c.drops_timeout; break;
    case skycue::DropReason::filtered: ++c.drops_filtered; break;
    case skycue::DropReason::source_lost: ++c.drops_source_lost; break;
    default: break;
    }
    if (reason != skycue::DropReason::filtered || !decision)
        return;
    switch (*decision) {
    case Decision::blocked: ++c.drops_blocked; break;
    case Decision::out_of_range: ++c.drops_out_of_range; break;
    case Decision::below_elevation: ++c.drops_below_elevation; break;
    case Decision::no_position: ++c.drops_no_position; break;
    default: break;  // send, allow_listed: never a drop
    }
}

// One filter change (Engine::filterHistory): the new state only, never a
// diff. The list changes are not recorded here.
struct FilterChange {
    std::chrono::system_clock::time_point time;  // the data clock
    std::string origin;                          // "startup", "ui", "file:<name>", "save-baseline", ...
    FilterConfig filters;                        // the new state
};
constexpr std::size_t kFilterHistoryMax = 100;

// One track as the Engine sees it now.
struct TrackSnapshot {
    skycue::Track track;  // toTrack of the state (lat/lon 0 without a position), keyframe false
    Decision decision = Decision::no_position;  // the filters' decision now
    double range_nm = 0, elevation_deg = 0;     // NaN as in FilterResult
    bool forwarded = false;                     // the consumer has it
    std::chrono::system_clock::time_point first_seen{}, last_seen{};  // receive times
    std::optional<std::string> emergency, category;
    double rssi = 0;  // dBFS; NaN when never reported
    std::string input;  // the newest observation's input id; empty when not known
    GeomResult geom;    // reported or estimated geometric altitude; track.alt_geom_ft is its value
};

struct Snapshot {
    // The data clock (the newest observation or uplink receive time plus the
    // steady time since; the wall clock before any). Compare it with
    // first_seen/last_seen, which are on the same clock.
    std::chrono::system_clock::time_point now{};
    std::vector<TrackSnapshot> tracks;  // every track in the table, in no particular order
    FilterConfig filters{0, 0};
    size_t allow_entries = 0, block_entries = 0;
    EngineStats stats;
    fisb::FisbSummary fisb;  // counts only; Engine::fisbSnapshot() has the products
};

enum class EventKind {
    update,
    observed,
    drop,
    expired,
    stats,
    filters_changed,
    lists_changed,
    fisb_text,     // a 413 text record, or a TWGO record without graphics, new or changed
    fisb_shape,    // a TWGO record with graphics, new or changed
    fisb_radar,    // a NEXRAD (63/64) mosaic was published
    fisb_raster,   // another raster layer (70/71/84/90/91/103) was published
    fisb_expired,  // a FIS-B item left the store, or a TWGO record was cancelled
    inputs_changed,  // an input was added, changed, removed, enabled, disabled, connected or lost
    fisb_location,   // a location id was learned, or its learned position moved (FisbNotice key, position)
    outputs_changed, // an output started, stopped, changed or was added or removed (read Engine::hub())
};

// What a fisb_* event carries. Radar and raster events are notifications:
// product, key, product time and block count, never the blocks (they are in
// Engine::fisbSnapshot() and the fisb recording).
struct FisbNotice {
    uint16_t product = 0;
    std::string key;  // the store key (fisb::textKey, TwgoKey::str(), Mosaic::key())
    std::optional<std::chrono::system_clock::time_point> product_time;  // radar/raster; text: issued
    std::size_t block_count = 0;                                        // radar/raster
    std::string reason;  // fisb_expired: "expired" | "cancelled"
    // fisb_expired: what left: text, twgo, radar or raster (fisb::FisbEvent::expired_kind).
    fisb::FisbEvent::Kind expired_kind = fisb::FisbEvent::Kind::text;
    std::optional<std::chrono::system_clock::time_point> expires;  // the item's expiry (fisb_expired: when due)
    std::optional<fisb::TextRecord> text;  // fisb_text from product 413; fisb_expired of a text record
    std::optional<fisb::TwgoRecord> twgo;  // fisb_text / fisb_shape from TWGO; fisb_expired "cancelled"
    std::optional<std::pair<double, double>> position;   // fisb_location: the learned position; key = the id
};

// update: `track` (the track just sent). observed: `track` (after an
// observation with a position, whatever the decision: every track, forwarded
// or not). drop: `id`, `reason` (any reason but shutdown), `callsign` (the
// track's last known one), `time` (the data clock at the drop) and, for
// reason filtered, `decision` (the filter decision that dropped it). expired: `id`
// (a track that was not being forwarded left the table: no drop is sent for
// it, but an observer showing it should remove it). stats: `stats`.
// filters_changed, lists_changed, inputs_changed, outputs_changed: nothing set (inputs_changed:
// read Engine::inputs(); outputs_changed: Engine::hub()). fisb_*: `fisb`.
struct EngineEvent {
    EventKind kind = EventKind::stats;
    std::optional<TrackSnapshot> track;
    std::optional<skycue::TrackId> id;
    std::optional<skycue::DropReason> reason;
    std::optional<Decision> decision;                           // drop, reason filtered: the decision that dropped it
    std::optional<std::string> callsign;                        // drop: the track's last known callsign
    std::optional<std::chrono::system_clock::time_point> time;  // drop: the data clock at the drop
    std::optional<EngineStats> stats;
    std::optional<FisbNotice> fisb;
};

}  // namespace skycued
