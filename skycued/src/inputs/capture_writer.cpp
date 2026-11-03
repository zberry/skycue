#include "inputs/capture_writer.hpp"

#include "log.hpp"
#include "retention.hpp"

#include <chrono>
#include <cstdio>
#include <set>
#include <utility>

namespace skycued {

namespace {

RotatingFileConfig fileConfig(const std::string& dir, const std::string& stream, const char* ext)
{
    RotatingFileConfig c;  // the Recorder's rotation: 256 MiB / 3600 s
    c.dir = dir;
    c.stream = stream;
    c.ext = ext;
    return c;
}

int64_t epochNs(std::chrono::system_clock::time_point t)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

}  // namespace

RecordSink::RecordSink(RotatingFileConfig config, std::function<void()> on_new_file)
    : file_(std::move(config)), on_new_file_(std::move(on_new_file))
{
}

bool RecordSink::write(const std::string& data)
{
    if (file_.rotationDue())
        file_.rotate();
    if (!file_.write(data.data(), data.size()))
        return false;
    const std::string now = file_.currentPath();
    if (now != last_path_) {  // a new file (not a failure streak's reopen of the same one)
        last_path_ = now;
        LOG_INFO("writing " << now);
        if (on_new_file_)
            on_new_file_();
    }
    return true;
}

CaptureWriter::CaptureWriter(std::string record_dir, std::string input_id, uint64_t max_bytes)
    : dir_(std::move(record_dir)), max_bytes_(max_bytes),
      sink_(fileConfig(dir_, "capture-" + input_id, "jsonl"), [this] {
          if (max_bytes_)
              enforce(dir_, max_bytes_, {sink_.currentPath()});
      })
{
}

bool CaptureWriter::write(const CaptureLine& line) { return sink_.write(captureLineJson(line) + "\n"); }

RawFrameWriter::RawFrameWriter(std::string record_dir, std::string input_id, uint64_t max_bytes)
    : dir_(std::move(record_dir)), max_bytes_(max_bytes),
      csv_(fileConfig(dir_, "raw-" + input_id, "csv"), [this] { enforceRetention(); }),
      txt_(fileConfig(dir_, "raw-" + input_id, "txt"), [this] { enforceRetention(); })
{
}

void RawFrameWriter::enforceRetention()
{
    if (max_bytes_ == 0)
        return;
    std::set<std::string> open_files;
    for (const RecordSink* s : {&csv_, &txt_})
        if (!s->currentPath().empty())
            open_files.insert(s->currentPath());
    enforce(dir_, max_bytes_, open_files);
}

void RawFrameWriter::write(const RawFrame& frame)
{
    const int64_t ns = epochNs(frame.time);
    if (ns < 0)
        return;  // before 1970: no line format for it
    char stamp[48];
    if (frame.kind == RawFrame::Kind::adsb_1090) {
        std::snprintf(stamp, sizeof stamp, "%lld.%09lld,", static_cast<long long>(ns / 1000000000),
                      static_cast<long long>(ns % 1000000000));
        std::string line = stamp;
        for (const char c : frame.hex)
            line += (c >= 'a' && c <= 'f') ? static_cast<char>(c - 'a' + 'A') : c;
        line += '\n';
        csv_.write(line);
    } else {
        std::snprintf(stamp, sizeof stamp, "%lld -", static_cast<long long>(ns / 1000000));
        txt_.write(stamp + frame.hex + ";\n");
    }
}

void RawFrameWriter::flush()
{
    csv_.flush();
    txt_.flush();
}

}  // namespace skycued
