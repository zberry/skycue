#pragma once

// The files a pingStation input writes itself (spec §5.4), each through a
// RotatingFile (256 MiB / 3600 s, as the Recorder) with retention::enforce
// run on RECORD_DIR each time a new file starts:
//
// - CaptureWriter (`capture: true`): RECORD_DIR/capture-<id>-YYYYMMDD-HHMMSS[-N].jsonl,
//   one captureLineJson line per datagram or framed block.
// - RawFrameWriter (RECORD_RAW=yes, JSON inputs): the rawData frames in the
//   line formats of skycue-rawrec, so skycue-rawplay replays them:
//   1090 to RECORD_DIR/raw-<id>-<stamp>.csv as "<epoch_s>.<9-digit ns>,<HEX>"
//   (parseCsvMessage; rawplay sends it to readsb's raw input), UAT to
//   RECORD_DIR/raw-<id>-<stamp>.txt as "<epoch_ms> -<hex>;" (parseUatLine;
//   rawplay serves it as dump978 raw).
//
// Write failures follow RotatingFile (data dropped with a backoff); the
// inputs count each dropped line or frame in capture_dropped. Not
// thread-safe; files are opened on the first write.

#include "asterix/capture_line.hpp"
#include "inputs/ping_json.hpp"
#include "rotating_file.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace skycued {

// What makeInput hands a pingStation input from the daemon's settings.
struct RecordOptions {
    std::string record_dir;   // "" = nothing is written
    bool record_raw = false;  // RECORD_RAW
    uint64_t max_bytes = 0;   // RECORD_MAX_GB in bytes; 0 = no retention
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
    CaptureWriter(std::string record_dir, std::string input_id, uint64_t max_bytes);
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
    RawFrameWriter(std::string record_dir, std::string input_id, uint64_t max_bytes);
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

}  // namespace skycued
