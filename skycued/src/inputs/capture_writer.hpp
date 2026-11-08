#pragma once

// The files a pingStation input writes itself (spec §5.4), each through a
// RotatingFile (256 MiB / 3600 s, as the Recorder) with retention::enforce
// run on recording.dir each time a new file starts:
//
// - CaptureWriter (`capture: true`): recording.dir/capture-<id>-YYYYMMDD-HHMMSS[-N].jsonl,
//   one captureLineJson line per datagram or framed block.
// - RawFrameWriter (recording.raw=yes, JSON inputs): the rawData frames in the
//   line formats of skycue-rawrec, so skycue-rawplay replays them:
//   1090 to recording.dir/raw-<id>-<stamp>.csv as "<epoch_s>.<9-digit ns>,<HEX>"
//   (parseCsvMessage; rawplay sends it to readsb's raw input), UAT to
//   recording.dir/raw-<id>-<stamp>.txt as "<epoch_ms> -<hex>;" (parseUatLine;
//   rawplay serves it as dump978 raw).
//
// - BeastCaptureWriter (`capture: true` on a beast_raw input): the Beast
//   frames as published, recording.dir/raw-<id>-<stamp>.bin (what skycue-rawplay
//   plays as a .bin; an AVR input is re-encoded, so the file is always Beast).
// - UatCaptureWriter (`capture: true` on a uat_raw input):
//   recording.dir/raw-<id>-<stamp>.txt, one "<epoch_ms> <line>\n" per line as
//   skycue-rawrec writes it (parseUatLine reads it back).
//
// Write failures follow RotatingFile (data dropped with a backoff); the
// inputs count each dropped line or frame in capture_dropped. Not
// thread-safe; files are opened on the first write.

#include "asterix/capture_line.hpp"
#include "inputs/ping_json.hpp"
#include "rotating_file.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace skycued {

// What makeInput hands a pingStation input from the daemon's settings.
struct RecordOptions {
    std::string record_dir;   // "" = nothing is written
    bool record_raw = false;  // recording.raw
    uint64_t max_bytes = 0;   // recording.max_gb in bytes; 0 = no retention
};

// One rotating output: rotates when due, notices each new file (logged at
// INFO) and calls on_new_file for it.
class RecordSink {
public:
    RecordSink(RotatingFileConfig config, std::function<void()> on_new_file);
    bool write(const std::string& data);
    void flush() { file_.flush(); }
    std::string currentPath() const { return file_.currentPath(); }
    uint64_t droppedBytes() const { return file_.droppedBytes(); }

private:
    RotatingFile file_;
    std::function<void()> on_new_file_;
    std::string last_path_;
};

class CaptureWriter {
public:
    CaptureWriter(std::string record_dir, const std::string& input_id, uint64_t max_bytes);
    bool write(const CaptureLine& line);  // false when dropped
    void flush() { sink_.flush(); }
    std::string currentPath() const { return sink_.currentPath(); }
    uint64_t droppedBytes() const { return sink_.droppedBytes(); }

private:
    std::string dir_;
    uint64_t max_bytes_;
    RecordSink sink_;
};

class RawFrameWriter {
public:
    RawFrameWriter(std::string record_dir, const std::string& input_id, uint64_t max_bytes);
    // False when dropped by a failed write (a frame before 1970 has no line
    // format and is skipped: true).
    bool write(const RawFrame& frame);
    void flush();

private:
    void enforceRetention();

    std::string dir_;
    uint64_t max_bytes_;
    RecordSink csv_, txt_;
};

// A rotating single-file capture with retention on each new file; the base of
// the two writers below.
class RawCaptureFile {
public:
    RawCaptureFile(std::string record_dir, const std::string& input_id, uint64_t max_bytes, const char* ext);
    RawCaptureFile(const RawCaptureFile&) = delete;
    RawCaptureFile& operator=(const RawCaptureFile&) = delete;
    void flush() { sink_.flush(); }
    std::string currentPath() const { return sink_.currentPath(); }
    uint64_t droppedBytes() const { return sink_.droppedBytes(); }

protected:
    bool put(const std::string& data) { return sink_.write(data); }

private:
    std::string dir_;
    uint64_t max_bytes_;
    RecordSink sink_;
};

class BeastCaptureWriter : public RawCaptureFile {
public:
    BeastCaptureWriter(std::string record_dir, const std::string& input_id, uint64_t max_bytes);
    bool write(std::string_view frame);  // false when dropped
};

class UatCaptureWriter : public RawCaptureFile {
public:
    UatCaptureWriter(std::string record_dir, const std::string& input_id, uint64_t max_bytes);
    // False when dropped by a failed write (a time before 1970 has no line
    // format and is skipped: true).
    bool write(std::chrono::system_clock::time_point time, std::string_view line);
};

}  // namespace skycued
