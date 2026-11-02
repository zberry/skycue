#pragma once

// What the Engine exposes to an observer (the web server): its counters, a
// snapshot of every track, and the events it emits as things happen.

#include "filters.hpp"
#include "fisb/fisb_store.hpp"

#include <skycue/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skycued {

struct EngineStats {
    uint64_t observations = 0, sent_updates = 0, sent_drops = 0, tracks_total = 0, tracks_forwarded = 0,
             uat_downlink = 0, uat_uplink = 0;
    bool readsb_connected = false, dump978_connected = false;
    uint64_t tcp_clients = 0, udp_targets = 0;  // Forwarder destinations now
    // sent_drops by reason (the shutdown drops are counted in sent_drops only).
    uint64_t drops_timeout = 0, drops_filtered = 0, drops_source_lost = 0;
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
};

// One track as the Engine sees it now.
struct TrackSnapshot {
    skycue::Track track;  // toTrack of the state (lat/lon 0 without a position), keyframe false
    Decision decision = Decision::no_position;  // the filters' decision now
    double range_nm = 0, elevation_deg = 0;     // NaN as in FilterResult
    bool forwarded = false;                     // the consumer has it
    std::chrono::system_clock::time_point first_seen{}, last_seen{};  // receive times
    std::optional<std::string> emergency, category;
    double rssi = 0;  // dBFS; NaN when never reported
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
};

// update: `track` (the track just sent). observed: `track` (after an
// observation with a position, whatever the decision: every track, forwarded
// or not). drop: `id` and `reason` (any reason but shutdown). expired: `id`
// (a track that was not being forwarded left the table: no drop is sent for
// it, but an observer showing it should remove it). stats: `stats`.
// filters_changed, lists_changed: nothing set. fisb_*: `fisb`.
struct EngineEvent {
    EventKind kind = EventKind::stats;
    std::optional<TrackSnapshot> track;
    std::optional<skycue::TrackId> id;
    std::optional<skycue::DropReason> reason;
    std::optional<EngineStats> stats;
    std::optional<FisbNotice> fisb;
};

}  // namespace skycued
