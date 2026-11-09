#pragma once

// ReplaySource: plays back a received-stream JSONL recording (see
// docs/recording-format.md) as TrackObservations on an io_context, paced by
// the records' "time" values.
//
// The whole file is read at construction (recordings are bounded by the
// recorder's rotation). Lines that are not JSON objects, "obs" records that
// do not parse, and unknown record types are counted, skipped, and reported
// in one WARN per file. "uat_uplink" records are played too, paced with the
// observations, through onUplink with their recorded "time" (shifted like
// observations in loop mode); one without "uplink_hex" is skipped silently.
//
// A line that is a readsb JSON-port aircraft object (a numeric "now" and no
// "time", as in tests/data/readsb-sample.jsonl) is read with
// parseReadsbLine instead, so a capture of readsb's output replays too.
//
// Threading: lives on the caller's io_context; call everything from its
// thread. Destroying the source stops it (pending handlers do nothing).

#include "observation.hpp"
#include "recording_reader.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace skycued {

// The wait from a replay's start to a record `offset` into the recording at
// `speed` (> 0): offset / speed seconds, clamped to [0,
// kMaxReplayDelaySeconds] (far enough out to be "never" while keeping
// steady_clock arithmetic safe); a negative offset (a record before the
// first, in a hand-edited file) or a NaN gives 0. Pure; ReplaySource paces
// with it.
constexpr double kMaxReplayDelaySeconds = 1e9;
std::chrono::steady_clock::duration replayDelay(std::chrono::system_clock::duration offset, double speed);

// t - first, saturated to system_clock::duration's range: a recording may
// hold any time the nanosecond clock can (1677 to 2262), and the difference
// of two such times can exceed an int64 count of nanoseconds. Pure;
// ReplaySource takes every offset and gap with it.
std::chrono::system_clock::duration replayOffset(std::chrono::system_clock::time_point t,
                                                 std::chrono::system_clock::time_point first);

// shift * pass, saturated to system_clock::duration's range: loop mode
// shifts pass N's timestamps by N times the recording's span, so the product
// may not wrap. Pure; ReplaySource takes every loop shift with it.
std::chrono::system_clock::duration replayLoopShift(std::chrono::system_clock::duration shift, uint64_t pass);

class ReplaySource {
public:
    // speed: 1.0 = real time; N = N times faster; 0 = no pacing (every
    // record is posted as soon as the io_context gets to it). Negative or
    // non-finite speed throws std::invalid_argument; a file that cannot be
    // opened throws std::runtime_error.
    // loop: at the end start again from the first record with every time
    // shifted forward by the recording's span plus its median gap between
    // records (at least 1 ms), so times keep increasing; onEnd never fires.
    ReplaySource(boost::asio::io_context& io, std::string path, double speed = 1.0, bool loop = false);
    ~ReplaySource();
    ReplaySource(const ReplaySource&) = delete;
    ReplaySource& operator=(const ReplaySource&) = delete;

    void onObservation(std::function<void(const TrackObservation&)> handler);
    void onUplink(std::function<void(std::chrono::system_clock::time_point, const std::string& hex)> handler);
    void onEnd(std::function<void()> handler);  // only when loop == false
    void start();                               // once

    std::size_t recordCount() const { return observations_; }    // observations loaded
    std::size_t uplinkCount() const { return records_.size() - observations_; }  // uplinks loaded
    std::size_t skippedLines() const { return skipped_; }        // bad lines (not uplinks or blanks)

private:
    void scheduleNext();
    void emitNext();

    boost::asio::io_context& io_;
    boost::asio::steady_timer timer_;
    std::string path_;
    double speed_;
    bool loop_;
    std::vector<RecordingRecord> records_;  // obs and uat_uplink records, in file order
    std::size_t observations_ = 0;
    std::size_t skipped_ = 0;
    std::chrono::system_clock::duration loop_shift_{};  // added per completed pass

    std::function<void(const TrackObservation&)> on_observation_;
    std::function<void(std::chrono::system_clock::time_point, const std::string&)> on_uplink_;
    std::function<void()> on_end_;

    std::size_t next_ = 0;   // index into records_
    uint64_t pass_ = 0;      // completed passes (loop mode)
    std::chrono::steady_clock::time_point start_{};
    bool started_ = false;
    std::shared_ptr<char> alive_ = std::make_shared<char>(0);
};

// ReplaySource as the Engine's ObservationSource: reports connected at
// start() and lost when a non-looping replay ends, so the Engine handles a
// finished replay like a lost readsb (tracks are dropped as source_lost
// after cues.track_timeout_s). stop() destroys the ReplaySource.
class ReplayObservationSource : public ObservationSource {
public:
    // Throws as ReplaySource's constructor does.
    ReplayObservationSource(boost::asio::io_context& io, std::string path, double speed = 1.0, bool loop = false);

    void onObservation(std::function<void(const TrackObservation&)> handler) override;
    void onConnection(std::function<void(bool)> handler) override;
    void onUplink(std::function<void(std::chrono::system_clock::time_point, const std::string& hex)> handler) override;
    void start() override;
    void stop() override;

    std::size_t recordCount() const { return count_; }

private:
    std::unique_ptr<ReplaySource> replay_;
    std::size_t count_ = 0;
    std::function<void(const TrackObservation&)> on_observation_;
    std::function<void(bool)> on_connection_;
    std::function<void(std::chrono::system_clock::time_point, const std::string&)> on_uplink_;
    bool started_ = false;
};

}  // namespace skycued
