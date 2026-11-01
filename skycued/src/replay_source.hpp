#pragma once

// ReplaySource: plays back a received-stream JSONL recording (see
// docs/recording-format.md) as TrackObservations on an io_context, paced by
// the records' "time" values.
//
// The whole file is read at construction (recordings are bounded by the
// recorder's rotation). Lines that are not JSON objects, "obs" records that
// do not parse, and unknown record types are counted, skipped, and reported
// in one WARN per file; "uat_uplink" records are skipped silently.
//
// Threading: lives on the caller's io_context; call everything from its
// thread. Destroying the source stops it (pending handlers do nothing).

#include "observation.hpp"

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
    void onEnd(std::function<void()> handler);  // only when loop == false
    void start();                               // once

    std::size_t recordCount() const { return records_.size(); }  // observations loaded
    std::size_t skippedLines() const { return skipped_; }        // bad lines (not uplinks or blanks)

private:
    struct Record {
        std::chrono::system_clock::time_point time;  // the record's "time"
        TrackObservation obs;
    };

    void scheduleNext();
    void emitNext();

    boost::asio::io_context& io_;
    boost::asio::steady_timer timer_;
    std::string path_;
    double speed_;
    bool loop_;
    std::vector<Record> records_;
    std::size_t skipped_ = 0;
    std::chrono::system_clock::duration loop_shift_{};  // added per completed pass

    std::function<void(const TrackObservation&)> on_observation_;
    std::function<void()> on_end_;

    std::size_t next_ = 0;   // index into records_
    uint64_t pass_ = 0;      // completed passes (loop mode)
    std::chrono::steady_clock::time_point start_{};
    bool started_ = false;
    std::shared_ptr<char> alive_ = std::make_shared<char>(0);
};

}  // namespace skycued
