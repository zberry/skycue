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
#include "recording_reader.hpp"

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
    // Reopen backoff after a failure; see Recorder. Settable for tests.
    std::chrono::milliseconds reopen_initial{5000};
    std::chrono::milliseconds reopen_max{300000};
};

class Recorder {
public:
    static constexpr std::size_t kFlushEveryRecords = 100;
    // After a write or open failure (a full disk, say), records are dropped
    // until a reopen is due: RecorderConfig::reopen_initial (5 s) after the
    // failure, doubling after each failed reopen up to reopen_max (300 s);
    // a successful flush ends the streak and resets the delay. The first
    // reopen of a streak starts a fresh file pair (unless the streak began
    // with a pair that could not be opened, which is then retried); every
    // later reopen in the streak appends to that same pair, so a persistent
    // failure neither floods the log nor creates files without limit: at most
    // one extra pair per stream.
    static constexpr std::chrono::seconds kReopenInterval{5};  // default reopen_initial

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
    // Any other event than "update" or "keyframe" throws std::invalid_argument.
    void writeTrack(const skycue::Track& track, const char* event /* "update" | "keyframe" */);
    void writeDrop(skycue::TrackId id, skycue::DropReason reason);
    void writeFilterChange(const std::string& description);

    void flush();
    // Path of the file currently written for "jsonl" or "csv" (a leading '.'
    // is accepted); "" when that format is disabled.
    std::string currentPath(const char* ext) const;

    // Records not written because the files could not be created or written
    // (records still in the write buffer when a failure hits are not counted).
    uint64_t droppedRecords() const { return dropped_; }
    // The current reopen delay during a failure streak; 0 when not failing.
    std::chrono::milliseconds reopenDelay() const { return failing_ ? reopen_delay_ : std::chrono::milliseconds(0); }
    // Test seam: while set, every write and flush fails as on a full disk.
    void setFailWritesForTest(bool fail) { fail_writes_for_test_ = fail; }

private:
    void fail(const std::string& what);  // starts or extends a failure streak; closes the files
    bool streamsGood() const;
    void flushStreams();
    void write(const std::vector<recording::Field>& record);
    // New files with a fresh name, or (reuse) stem_ again, appended to; false
    // (errno set) on failure.
    bool openFiles(bool reuse = false);
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
    std::string last_base_;  // name stem without suffix, and the suffix used last
    int last_suffix_ = 0;

    // Failure streak: one ERROR when it starts, one INFO when a flush succeeds again.
    bool failing_ = false;
    std::chrono::steady_clock::time_point next_reopen_{};
    std::chrono::milliseconds reopen_delay_{0};
    bool streak_has_stem_ = false;  // later reopens in this streak reuse stem_
    uint64_t dropped_ = 0, streak_start_dropped_ = 0;
    bool fail_writes_for_test_ = false;
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

// Fills `out` (reset first) from a sent-stream record: "time" and a known
// "event" are required; update/keyframe need "hex", drop needs "hex" and a
// known "drop_reason". false when any of that is missing or a present field
// has the wrong form or range. `out.file` is 0.
bool sentRecordFromRecord(const JsonObject& record, RecordingRecord& out);

// The enum texts both formats use ("adsb_1090", "non_icao", "source_lost", ...).
const char* addressTypeName(skycue::AddressType type);
const char* sourceName(skycue::Source source);
const char* dropReasonName(skycue::DropReason reason);

}  // namespace recording

}  // namespace skycued
