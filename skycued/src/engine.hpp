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
// observation's receive time plus the steady time since it, so a replay of
// old data ages like live data); forwarded tracks among the expired get
// sendDrop(timeout), or sendDrop(source_lost) while the source is down; both
// recorders are flushed. When the source has been lost for TRACK_TIMEOUT_S,
// every forwarded track is dropped as source_lost (a reconnect cancels it).
// The keyframe provider returns every forwarded track (periodic keyframes
// are not recorded). Stats are logged at INFO every 10 s.
//
// Threading: lives on the caller's io_context; call everything from its
// thread. The Forwarder and the recorders must outlive the Engine.

#include "config.hpp"
#include "dump978_raw_source.hpp"
#include "filters.hpp"
#include "forwarder.hpp"
#include "observation.hpp"
#include "recorder.hpp"
#include "track_table.hpp"

#include "change_detector.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace skycued {

struct EngineStats {
    uint64_t observations = 0, sent_updates = 0, sent_drops = 0, tracks_total = 0, tracks_forwarded = 0,
             uat_downlink = 0, uat_uplink = 0;
    bool readsb_connected = false, dump978_connected = false;
};

class Engine {
public:
    // Loads the allow/block lists named in the config: throws
    // std::runtime_error when one cannot be read or parsed, or when the
    // receiver position is missing. `uat` may be null; so may the recorders.
    Engine(boost::asio::io_context& io, const Config& config, std::unique_ptr<ObservationSource> source,
           std::unique_ptr<Dump978RawSource> uat, Forwarder& forwarder, Recorder* received, Recorder* sent);
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

private:
    void handleObservation(const TrackObservation& obs);
    void handleConnection(bool up);
    void process(TrackState& state, std::chrono::steady_clock::time_point now);
    void drop(TrackState& state, skycue::DropReason reason);
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

    // Before filters_: makeFilters() sets them while filters_ is initialised.
    size_t allow_size_ = 0, block_size_ = 0;
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
    std::shared_ptr<char> alive_ = std::make_shared<char>(0);
};

}  // namespace skycued
