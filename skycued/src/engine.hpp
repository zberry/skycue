#pragma once

// Engine: wires the inputs (an InputManager: readsb, dump978, pingStations,
// a replay) to the Forwarder through the track table, change detection and
// filters, and records both streams.
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
// sendDrop(timeout), or sendDrop(source_lost) while the inputs are lost; both
// recorders are flushed.
//
// Connection (spec §4.4): the inputs are lost when no enabled track input
// (any type but dump978) is connected, connected meaning the transport is up
// (a bound UDP socket is connected even when nothing arrives: its tracks just
// time out). When they have been lost for TRACK_TIMEOUT_S, every forwarded
// track is dropped as source_lost; any track input connecting again cancels
// it. One input going while another stays leaves its tracks to time out.
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
// updates it causes, inputs_changed after any change the InputManager
// reports (definitions, enabled, connection; not device status). Fired
// synchronously on the io thread; never after stop().
//
// Uplinks: one whose hex came from a different input within the last 2 s
// (UplinkDedupe) is counted in fisb_uplink_duplicates and dropped: neither
// recorded nor decoded.
//
// FIS-B (FISB_ENABLE=yes, the default): every other uplink, from any input
// (dump978's raw port, a pingStation, a replay's "uat_uplink" records), is
// recorded in the received stream, moves the data clock forward when it is newer, and is fed to a
// FisbDecoder with its own receive time as the reference time; the products
// go to a FisbStore at the data clock. The 1 s tick sweeps both on the data
// clock (so a replay of 2015 uplinks keeps 2015 products for their 2015
// lifetimes). Each FisbEvent becomes one fisb-stream record (Recorder::
// writeFisb, "time" = the data clock) and one fisb_text / fisb_shape /
// fisb_radar / fisb_raster / fisb_expired event (FisbNotice).
//
// Locations (design §3.2): every TWGO record with graphics is fed to a
// fisb::LocationIndex; an id learned for the first time, or a learned
// position that moves by 1e-4 degrees or more, emits fisb_location (after
// the record's own event). The learned set is saved (atomicWrite) to the
// loadLocations file on the 1 s tick when it changed and
// kLocationsSaveInterval has passed since the last attempt, and by stop();
// a failed save logs one WARN per run of failures and is retried.
//
// Threading: lives on the caller's io_context; call everything (including
// every method below) from its thread. The Forwarder and the recorders must
// outlive the Engine.

#include "config.hpp"
#include "engine_events.hpp"
#include "filters.hpp"
#include "forwarder.hpp"
#include "inputs/input_manager.hpp"
#include "observation.hpp"
#include "recorder.hpp"
#include "track_table.hpp"

#include "change_detector.hpp"
#include "fisb/fisb_decoder.hpp"
#include "fisb/fisb_store.hpp"
#include "fisb/location_index.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace skycued {

constexpr std::chrono::seconds kLocationsSaveInterval{60};

class Engine {
public:
    // Loads the allow/block lists named in the config: throws
    // std::runtime_error when one cannot be read or parsed, or when the
    // receiver position is missing; std::invalid_argument when `inputs` is
    // null. The recorders may be null (`fisb` is the "fisb" stream). The
    // Engine registers the manager's callbacks: register none of your own.
    Engine(boost::asio::io_context& io, const Config& config, std::unique_ptr<InputManager> inputs,
           Forwarder& forwarder, Recorder* received, Recorder* sent, Recorder* fisb = nullptr);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts the Forwarder (throws std::runtime_error when its TCP port
    // cannot be bound; the Engine is then left unstarted), writes the startup
    // filter_change record, starts the inputs and the timers. Once.
    void start();
    // Stops the inputs and timers, records a shutdown drop for every
    // forwarded track, stops the Forwarder (which sends them), flushes the
    // recorders. Idempotent.
    void stop();

    EngineStats stats() const;
    // "stats obs=N sent=N drops=N tracks=N fwd=N uat_dl=N uat_ul=N
    // readsb=up|down dump978=up|down|off inputs=<connected>/<enabled>"
    std::string statsLine() const;

    InputManager& inputs() { return *inputs_; }
    const InputManager& inputs() const { return *inputs_; }

    // Every track in the table with its filter decision now, the filters and
    // the stats. Changes nothing.
    Snapshot snapshot() const;
    // The current FIS-B products (empty with FISB_ENABLE=no): a copy of
    // everything, every mosaic's blocks included.
    fisb::FisbSnapshot fisbSnapshot() const;
    // fisbSnapshot() without the mosaics (FisbStore::records()).
    fisb::FisbSnapshot fisbRecords() const;
    // Counts and mosaic heads only (what snapshot().fisb holds), cheap.
    fisb::FisbSummary fisbSummary() const;
    // One mosaic with its blocks (FisbStore::mosaic); nullopt when none.
    std::optional<fisb::Mosaic> fisbMosaic(uint16_t product, std::optional<uint8_t> altitude_code) const;
    // FisbStore::blocksCopied (a diagnostic for tests).
    uint64_t fisbBlocksCopied() const;

    // Loads the learned file (missing: empty; malformed: one ERROR line, renamed "<path>.rejected", empty) and the
    // operator CSV (empty path: none; unreadable: throws std::runtime_error naming it; rejected lines: one WARN with
    // the count and the first line number). Call before start(). Remembers learned_path for saving ("": never saved).
    void loadLocations(const std::string& learned_path, const std::string& operator_csv_path);
    // Every known location id with its position, learned over operator (LocationIndex::merged()).
    std::map<std::string, std::pair<double, double>> locationPositions() const;

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
    // The manager reported a change: track_connected_ follows
    // anyTrackInputConnected() (arming or cancelling lost_timer_), then
    // inputs_changed is emitted.
    void inputsChanged();
    // The track inputs were just lost: arms lost_timer_ (source_lost drops).
    void armLost();
    // No built-in dump978, or it is disabled.
    bool dump978Off() const;
    void handleUplink(std::chrono::system_clock::time_point when, const std::string& hex);
    void handleFisbEvents(std::vector<fisb::FisbEvent>& events);
    // Feeds a TWGO record to the location index; the fisb_location event to emit, if any.
    std::optional<EngineEvent> learnLocation(const fisb::TwgoRecord& record);
    // Writes the learned locations now (the caller checks dirty()).
    void saveLocations();
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
    // Destroyed after every member below it; safe because ~Engine calls stop() first (no input callback runs after).
    std::unique_ptr<InputManager> inputs_;
    UplinkDedupe dedupe_;
    Forwarder& forwarder_;
    Recorder* received_;
    Recorder* sent_;
    Recorder* fisb_rec_;
    std::unique_ptr<fisb::FisbDecoder> fisb_decoder_;  // null with FISB_ENABLE=no
    fisb::FisbStore fisb_store_;
    std::optional<std::chrono::system_clock::time_point> last_uplink_;
    fisb::LocationIndex locations_;
    std::string locations_path_;  // fisb-locations.json; "" until loadLocations
    std::chrono::steady_clock::time_point last_locations_save_{};  // the last save attempt (or start())
    bool locations_save_failing_ = false;                           // the last save attempt failed

    FilterSet filters_;
    Thresholds thresholds_;
    std::chrono::seconds track_timeout_;
    TrackTable table_;

    boost::asio::steady_timer tick_timer_, stats_timer_, lost_timer_;
    bool started_ = false, stopped_ = false;
    bool track_connected_ = false;  // an enabled track input is connected
    bool have_data_clock_ = false;
    std::chrono::system_clock::time_point last_obs_time_{};
    std::chrono::steady_clock::time_point last_obs_steady_{};
    unsigned lost_gen_ = 0;
    uint64_t observations_ = 0, sent_updates_ = 0, sent_drops_ = 0;
    uint64_t drops_timeout_ = 0, drops_filtered_ = 0, drops_source_lost_ = 0;
    uint64_t uplink_duplicates_ = 0;
    uint32_t web_clients_ = 0;
    std::function<void(const EngineEvent&)> sink_;
    std::shared_ptr<char> alive_ = std::make_shared<char>(0);
};

}  // namespace skycued
