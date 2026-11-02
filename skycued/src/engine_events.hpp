#pragma once

// What the Engine exposes to an observer (the web server): its counters, a
// snapshot of every track, and the events it emits as things happen.

#include "filters.hpp"

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
    // The data clock (the newest observation's receive time plus the steady
    // time since; the wall clock before any observation). Compare it with
    // first_seen/last_seen, which are on the same clock.
    std::chrono::system_clock::time_point now{};
    std::vector<TrackSnapshot> tracks;  // every track in the table, in no particular order
    FilterConfig filters{0, 0};
    size_t allow_entries = 0, block_entries = 0;
    EngineStats stats;
};

enum class EventKind { update, observed, drop, stats, filters_changed, lists_changed };

// update: `track` (the track just sent). observed: `track` (after an
// observation with a position, whatever the decision: every track, forwarded
// or not). drop: `id` and `reason` (any reason but shutdown). stats: `stats`. filters_changed, lists_changed: nothing set.
struct EngineEvent {
    EventKind kind = EventKind::stats;
    std::optional<TrackSnapshot> track;
    std::optional<skycue::TrackId> id;
    std::optional<skycue::DropReason> reason;
    std::optional<EngineStats> stats;
};

}  // namespace skycued
