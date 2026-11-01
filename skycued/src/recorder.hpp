#pragma once

// Recorder: writes one stream ("received" observations and UAT uplinks, or
// "sent" updates/keyframes/drops/filter changes) as JSONL and/or CSV files
// under a directory, rotating by size and age. docs/recording-format.md
// describes the files; the key table in recorder.cpp is the one source for
// both the writer and ReplaySource's reader.
//
// Threading: not thread-safe; use from the Engine's io_context thread.
// Writes are synchronous and buffered: flush() pushes them to the files, and
// the recorder flushes by itself every kFlushEveryRecords records, on
// rotation and on destruction.

#include "json_lite.hpp"
#include "observation.hpp"

#include <skycue/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace skycued {

namespace recording {
// One present value of a record (absent optionals produce no Field).
struct Field {
    enum class Kind { text, integer, real, real_float, boolean };
    const char* key = "";
    Kind kind = Kind::text;
    std::string text;
    int64_t integer = 0;
    double real = 0;
    float real_float = 0;
    bool boolean = false;
};
}  // namespace recording

struct RecorderConfig {
    std::string dir;  // created (mkdir -p) when missing
    bool jsonl = true, csv = true;
    // Checked before each write: when either current file has reached
    // rotate_bytes, or the files are rotate_after old, both are closed and a
    // new pair is started. 0 disables that check.
    uint64_t rotate_bytes = 256ull << 20;
    std::chrono::seconds rotate_after{3600};
};

class Recorder {
public:
    static constexpr std::size_t kFlushEveryRecords = 100;

    // stream_name is "received" or "sent" (it picks the CSV columns);
    // anything else throws std::invalid_argument. Throws std::runtime_error
    // when the directory cannot be created or a file cannot be opened.
    Recorder(RecorderConfig config, std::string stream_name);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Received stream.
    void writeObservation(const TrackObservation& obs);  // "type":"obs", "time" = received_time
    void writeUplink(std::chrono::system_clock::time_point when, const std::string& hex);  // "type":"uat_uplink"

    // Sent stream; "time" is the wall clock at the call.
    void writeTrack(const skycue::Track& track, const char* event /* "update" | "keyframe" */);
    void writeDrop(skycue::TrackId id, skycue::DropReason reason);
    void writeFilterChange(const std::string& description);

    void flush();
    // Path of the file currently written for "jsonl" or "csv" (a leading '.'
    // is accepted); "" when that format is disabled.
    std::string currentPath(const char* ext) const;

private:
    void write(const std::vector<recording::Field>& record);
    bool openFiles();  // new files with a fresh name; false (errno set) on failure
    void closeFiles();
    bool rotationDue() const;

    RecorderConfig config_;
    std::string stream_;
    bool sent_ = false;
    std::string stem_;  // <dir>/<stream>-YYYYMMDD-HHMMSS[-N]
    std::ofstream jsonl_, csv_;
    bool open_ = false;
    uint64_t jsonl_bytes_ = 0, csv_bytes_ = 0;
    std::chrono::steady_clock::time_point opened_{};
    std::size_t unflushed_ = 0;
    bool error_logged_ = false;  // one ERROR per failure streak
};

// The record format, shared with ReplaySource.
namespace recording {

// Keys of an "obs" record after "time" and "type", in the order written.
std::vector<std::string> observationKeys();
// Keys of a sent-stream record after "time" and "event".
std::vector<std::string> sentKeys();
std::string receivedCsvHeader();
std::string sentCsvHeader();

// Rebuilds an observation from a received-stream record whose "type" is
// "obs" ("raw" becomes raw_line when present). nullopt when "time" or "hex"
// is missing or any present field has the wrong form or range.
std::optional<TrackObservation> observationFromRecord(const JsonObject& record);

}  // namespace recording

}  // namespace skycued
