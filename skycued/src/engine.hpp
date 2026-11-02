#pragma once

// Engine: wires a source of observations to the Forwarder through the track
// table, change detection and filters, and records both streams.
//
// Per observation: received->writeObservation, table.apply, then
//   filters.evaluate when shouldSend gives a reason, or the track has a
//   position and is forwarded (so leaving the area drops it at once), or is
//   not forwarded (at most once per MIN_INTERVAL_S since its last send);
//   - a send decision (with a reason, or for a track not forwarded):
//     Forwarder::sendUpdate(toTrack(state, false,
//     result.alt_is_geom)), markSent, sent->writeTrack("update"), forwarded;
//   - otherwise, if it was forwarded: sendDrop(filtered), writeDrop, no
//     longer forwarded (it is sent afresh once it is eligible again).
// Every second: table.expire(TRACK_TIMEOUT_S) on the data clock (the newest
// observation or uplink receive time seen, plus the steady time since; it
// never goes back; so a replay of old data ages like live data); forwarded tracks among the expired get
// sendDrop(timeout), or sendDrop(source_lost) while the source is down; both
// recorders are flushed. When the source has been lost for TRACK_TIMEOUT_S,
// every forwarded track is dropped as source_lost (a reconnect cancels it).
// The keyframe provider returns every forwarded track (periodic keyframes
// are not recorded). Stats are logged at INFO every 10 s.
//
// Live changes (setFilterConfig, setLists, addListEntry, removeListEntry):
// the change is applied, ONE filter_change record "<origin>: <what>" is
// written to the sent stream and logged at INFO, then every track in the
// table is re-evaluated at once, in one pass: a forwarded track that is no
// longer eligible gets sendDrop(filtered); one not forwarded that has a
// position and is now eligible is sent at once (not waiting for
// MIN_INTERVAL_S), through markSent like every send. So the record precedes
// the drops and updates it causes. The received stream never depends on the
// filters: every observation is recorded before any of this.
//
// Events (onEvent): update after every update sent; observed after every
// observation that carries a position, for every track (forwarded, blocked,
// out of range...), once its decision is made (so after the update or drop
// that observation caused), with the track's decision, range and elevation
// now; drop after every drop
// sent (timeout, filtered, source_lost; the shutdown drops of stop() are
// not reported), stats on every 1 s tick, filters_changed / lists_changed
// after a live change is applied and recorded but before the drops and
// updates it causes. Fired synchronously on the io thread; never after stop().
//
// FIS-B (FISB_ENABLE=yes, the default): every uplink, from dump978's raw port
// or from a replay's "uat_uplink" records, is recorded in the received
// stream, moves the data clock forward when it is newer, and is fed to a
// FisbDecoder with its own receive time as the reference time; the products
// go to a FisbStore at the data clock. The 1 s tick sweeps both on the data
// clock (so a replay of 2015 uplinks keeps 2015 products for their 2015
// lifetimes). Each FisbEvent becomes one fisb-stream record (Recorder::
// writeFisb, "time" = the data clock) and one fisb_text / fisb_shape /
// fisb_radar / fisb_raster / fisb_expired event (FisbNotice).
//
// Threading: lives on the caller's io_context; call everything (including
// every method below) from its thread. The Forwarder and the recorders must
// outlive the Engine.

#include "config.hpp"
#include "dump978_raw_source.hpp"
#include "engine_events.hpp"
#include "filters.hpp"
#include "forwarder.hpp"
#include "observation.hpp"
#include "recorder.hpp"
#include "track_table.hpp"

#include "change_detector.hpp"
#include "fisb/fisb_decoder.hpp"
#include "fisb/fisb_store.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace skycued {

class Engine {
public:
    // Loads the allow/block lists named in the config: throws
    // std::runtime_error when one cannot be read or parsed, or when the
    // receiver position is missing. `uat` may be null; so may the recorders
    // (`fisb` is the "fisb" stream).
    Engine(boost::asio::io_context& io, const Config& config, std::unique_ptr<ObservationSource> source,
           std::unique_ptr<Dump978RawSource> uat, Forwarder& forwarder, Recorder* received, Recorder* sent,
           Recorder* fisb = nullptr);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts the Forwarder (throws std::runtime_error when its TCP port
    // cannot be bound; the Engine is then left unstarted), writes the startup
    // filter_change record, starts the sources and the timers. Once.
    void start();
    // Stops the sources and timers, records a shutdown drop for every
    // forwarded track, stops the Forwarder (which sends them), flushes the
    // recorders. Idempotent.
    void stop();

    EngineStats stats() const;
    // "stats obs=N sent=N drops=N tracks=N fwd=N uat_dl=N uat_ul=N
    // readsb=up|down dump978=up|down|off"
    std::string statsLine() const;

    // Every track in the table with its filter decision now, the filters and
    // the stats. Changes nothing.
    Snapshot snapshot() const;
    // The current FIS-B products (empty with FISB_ENABLE=no).
    fisb::FisbSnapshot fisbSnapshot() const;

    // Live changes (see the top of this file). `origin` names who made the
    // change in the record ("ui", say). Ignored after stop().
    // Throws std::invalid_argument, changing nothing, unless max_range_nm is
    // finite and > 0 and min_elevation_deg is in [-90, 90].
    void setFilterConfig(FilterConfig config, const std::string& origin);
    void setLists(IdList allow, IdList block, const std::string& origin);
    // One entry, in the list file grammar. Throws std::invalid_argument for a
    // bad entry. Adding an entry already listed, or removing one that is not,
    // changes nothing: no record, no event.
    void addListEntry(ListKind list, const std::string& entry, const std::string& origin);
    void removeListEntry(ListKind list, const std::string& entry, const std::string& origin);
    const IdList& allowList() const { return filters_.allow(); }
    const IdList& blockList() const { return filters_.block(); }

    // The one event sink (replaces any earlier one; empty to clear). It must
    // not call the live-change methods or stop().
    void onEvent(std::function<void(const EngineEvent&)> sink);
    // Reported in EngineStats::web_clients.
    void setWebClientCount(uint32_t n) { web_clients_ = n; }

private:
    void handleObservation(const TrackObservation& obs);
    void handleConnection(bool up);
    void handleUplink(std::chrono::system_clock::time_point when, const std::string& hex);
    void handleFisbEvents(std::vector<fisb::FisbEvent>& events);
    // `positioned`: the observation carried a position (an observed event follows).
    void process(TrackState& state, std::chrono::steady_clock::time_point now, bool positioned);
    void decide(TrackState& state, std::chrono::steady_clock::time_point now);
    void forward(TrackState& state, const FilterResult& result, std::chrono::steady_clock::time_point now);
    void drop(TrackState& state, skycue::DropReason reason);
    void countDrop(skycue::DropReason reason);
    void applied(const std::string& note, EventKind kind);
    void reevaluate();
    TrackSnapshot trackSnapshot(const TrackState& state, const FilterResult& result) const;
    std::chrono::system_clock::time_point dataNow() const;
    void advanceDataClock(std::chrono::system_clock::time_point t);
    void emit(const EngineEvent& event);
    void emitDrop(skycue::TrackId id, skycue::DropReason reason);
    void dropAllForwarded(skycue::DropReason reason);
    void tick();
    void armTick();
    void armStats();
    std::string filterDescription() const;

    boost::asio::io_context& io_;
    Config config_;
    std::unique_ptr<ObservationSource> source_;
    std::unique_ptr<Dump978RawSource> uat_;
    Forwarder& forwarder_;
    Recorder* received_;
    Recorder* sent_;
    Recorder* fisb_rec_;
    std::unique_ptr<fisb::FisbDecoder> fisb_decoder_;  // null with FISB_ENABLE=no
    fisb::FisbStore fisb_store_;
    std::optional<std::chrono::system_clock::time_point> last_uplink_;

    FilterSet filters_;
    Thresholds thresholds_;
    std::chrono::seconds track_timeout_;
    TrackTable table_;

    boost::asio::steady_timer tick_timer_, stats_timer_, lost_timer_;
    bool started_ = false, stopped_ = false;
    bool source_connected_ = false, uat_connected_ = false;
    bool have_data_clock_ = false;
    std::chrono::system_clock::time_point last_obs_time_{};
    std::chrono::steady_clock::time_point last_obs_steady_{};
    unsigned lost_gen_ = 0;
    uint64_t observations_ = 0, sent_updates_ = 0, sent_drops_ = 0;
    uint64_t drops_timeout_ = 0, drops_filtered_ = 0, drops_source_lost_ = 0;
    uint32_t web_clients_ = 0;
    std::function<void(const EngineEvent&)> sink_;
    std::shared_ptr<char> alive_ = std::make_shared<char>(0);
};

}  // namespace skycued
