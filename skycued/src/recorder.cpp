// The Recorder class: files, rotation, retention and failure streaks. The
// recording format itself (keys, field builders, JSONL/CSV encoding) is in
// recording_codec.cpp.

#include "recorder.hpp"
#include "recording_codec.hpp"

#include "fisb/fisb_json.hpp"
#include "fisb/fisb_store.hpp"

#include "log.hpp"
#include "retention.hpp"
#include "time_util.hpp"
#include "track_names.hpp"

#include <sys/stat.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <utility>

namespace skycued {
// ---- Recorder ----

using recording::Field;
using recording::Record;

namespace {

std::string now8601() { return formatIso8601Ms(std::chrono::system_clock::now()); }

Record withHead(std::string time, const char* kind_key, std::string kind, Record body)
{
    Record r{recording::textField("time", std::move(time)), recording::textField(kind_key, std::move(kind))};
    r.insert(r.end(), std::make_move_iterator(body.begin()), std::make_move_iterator(body.end()));
    return r;
}

}  // namespace

Recorder::Recorder(RecorderConfig config, std::string stream_name)
    : config_(std::move(config)), stream_(std::move(stream_name)),
      backoff_(config_.reopen_initial, config_.reopen_max)
{
    if (stream_ != "received" && stream_ != "sent" && stream_ != "fisb")
        throw std::invalid_argument("recorder: stream must be \"received\", \"sent\" or \"fisb\", not \"" +
                                    stream_ + "\"");
    sent_ = stream_ == "sent";
    if (stream_ == "fisb")
        config_.csv = false;  // FIS-B is recorded as JSONL only
    if ((config_.jsonl || config_.csv) && !openFiles())
        throw std::runtime_error("recorder: cannot create " + stream_ + " files under \"" + config_.dir +
                                 "\": " + std::strerror(errno));
    enforceRetention();
}

Recorder::~Recorder() { closeFiles(); }

namespace {

// Size of an existing file, and whether it ends in a partial line (its last
// byte is not '\n'); 0 / false when it does not exist.
uint64_t existingSize(const std::string& path, bool& partial_line)
{
    partial_line = false;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || st.st_size <= 0)
        return 0;
    std::ifstream in(path, std::ios::binary);
    char last = '\n';
    if (in.seekg(-1, std::ios::end))
        in.get(last);
    partial_line = last != '\n';
    return static_cast<uint64_t>(st.st_size);
}

// Opens `path` for writing: truncated, or appended to with `reuse`. A reused
// file that ends mid-line (a write cut short by the failure) gets a newline
// first, so the next record starts on a line of its own. Sets `bytes` to the
// file's size.
bool openStream(std::ofstream& f, const std::string& path, bool reuse, std::atomic<uint64_t>& bytes_out)
{
    bool partial = false;
    uint64_t bytes = reuse ? existingSize(path, partial) : 0;
    f.open(path, std::ios::out | std::ios::binary | (reuse ? std::ios::app : std::ios::trunc));
    if (!f)
        return false;
    if (partial) {
        f << '\n';
        ++bytes;
    }
    bytes_out.store(bytes, std::memory_order_relaxed);
    return true;
}

}  // namespace

bool Recorder::openFiles(bool reuse)
{
    if (!makeDirs(config_.dir))
        return false;
    if (!reuse || stem_.empty()) {
        reuse = false;
        const std::string base = config_.dir + "/" + stream_ + "-" + formatCompactUtc(std::chrono::system_clock::now());
        stem_ = uniqueStem(base, {"jsonl", "csv"}, last_base_, last_suffix_);
    }
    jsonl_bytes_.store(0, std::memory_order_relaxed);
    csv_bytes_.store(0, std::memory_order_relaxed);
    if (config_.jsonl && !openStream(jsonl_, stem_ + ".jsonl", reuse, jsonl_bytes_))
        return false;
    if (config_.csv) {
        if (!openStream(csv_, stem_ + ".csv", reuse, csv_bytes_)) {
            const int err = errno;
            if (jsonl_.is_open()) {
                jsonl_.close();
                if (!reuse)
                    // NOLINTNEXTLINE(cert-err33-c): best effort: the open error is what gets reported
                    std::remove((stem_ + ".jsonl").c_str());  // leave no half pair behind
            }
            errno = err;
            return false;
        }
        if (csv_bytes_.load(std::memory_order_relaxed) == 0) {
            const std::string head = (sent_ ? recording::sentCsvHeader() : recording::receivedCsvHeader()) + "\n";
            csv_ << head;
            csv_bytes_.store(head.size(), std::memory_order_relaxed);
        }
    }
    opened_ = std::chrono::steady_clock::now();
    open_ = true;
    return true;
}

void Recorder::closeFiles()
{
    if (jsonl_.is_open())
        jsonl_.close();
    if (csv_.is_open())
        csv_.close();
    jsonl_.clear();
    csv_.clear();
    unflushed_ = 0;
    open_ = false;
}

bool Recorder::rotationDue() const
{
    if (config_.rotate_bytes > 0 && (jsonl_bytes_.load(std::memory_order_relaxed) >= config_.rotate_bytes ||
                                  csv_bytes_.load(std::memory_order_relaxed) >= config_.rotate_bytes))
        return true;
    return config_.rotate_after.count() > 0 && std::chrono::steady_clock::now() - opened_ >= config_.rotate_after;
}

void Recorder::fail(const std::string& what)
{
    closeFiles();
    if (!backoff_.fail())
        return;
    streak_start_dropped_ = dropped_;
    LOG_ERROR("recorder: " << what << "; dropping " << stream_ << " records, retrying in "
                           << std::chrono::duration<double>(backoff_.initial()).count() << " s, backing off to "
                           << std::chrono::duration<double>(backoff_.max()).count() << " s");
}

void Recorder::enforceRetention()
{
    if (config_.max_bytes == 0)
        return;
    std::set<std::string> open_files;
    for (const char* ext : {"jsonl", "csv"}) {
        const std::string p = currentPath(ext);
        if (!p.empty())
            open_files.insert(p);
    }
    enforce(config_.retention_dir.empty() ? config_.dir : config_.retention_dir, config_.max_bytes, open_files);
}

bool Recorder::streamsGood() const
{
    return !fail_writes_for_test_ && (!config_.jsonl || jsonl_) && (!config_.csv || csv_);
}

void Recorder::write(const Record& record)
{
    if (!beginWrite())
        return;
    if (config_.jsonl) {
        const std::string line = recording::jsonLine(record);
        jsonl_ << line;
        jsonl_bytes_.fetch_add(line.size(), std::memory_order_relaxed);
    }
    if (config_.csv) {
        const std::string line = recording::csvRow(sent_ ? recording::sentColumns() : recording::receivedColumns(),
                                                   record);
        csv_ << line;
        csv_bytes_.fetch_add(line.size(), std::memory_order_relaxed);
    }
    endWrite();
}

void Recorder::writeFisb(const fisb::FisbEvent& event, std::chrono::system_clock::time_point time)
{
    if (!beginWrite())
        return;
    if (config_.jsonl) {
        const std::string line = fisb::eventRecordJson(event, time) + '\n';
        jsonl_ << line;
        jsonl_bytes_.fetch_add(line.size(), std::memory_order_relaxed);
    }
    endWrite();
}

bool Recorder::beginWrite()
{
    if (!config_.jsonl && !config_.csv)
        return false;
    if (backoff_.waiting()) {
        ++dropped_;
        return false;
    }
    if (!open_ || rotationDue()) {
        closeFiles();
        // Within a failure streak, reopen the streak's pair rather than
        // starting yet another one.
        const bool reuse = backoff_.reuseName();
        const bool opened = openFiles(reuse);
        const int err = errno;
        backoff_.opened(opened);
        if (!opened) {
            fail("cannot create files under \"" + config_.dir + "\": " + std::strerror(err));
            ++dropped_;
            return false;
        }
        if (!reuse)
            enforceRetention();
    }
    if (fail_writes_for_test_) {
        fail("writing " + stem_ + " failed (test)");
        ++dropped_;
        return false;
    }
    return true;
}

void Recorder::endWrite()
{
    // While recovering, flush every record: the streak ends only once a
    // flush has really reached the files.
    if (++unflushed_ >= kFlushEveryRecords || backoff_.failing())
        flushStreams();
    if (!streamsGood()) {
        fail("writing " + stem_ + " failed: " + std::strerror(errno));
        ++dropped_;
        return;
    }
    if (backoff_.failing() && unflushed_ == 0) {
        backoff_.succeed();
        LOG_INFO("recorder: writing " << stream_ << " records again (" << stem_ << "); dropped "
                                      << dropped_ - streak_start_dropped_ << " meanwhile");
    }
}

void Recorder::flushStreams()
{
    if (jsonl_.is_open())
        jsonl_.flush();
    if (csv_.is_open())
        csv_.flush();
    unflushed_ = 0;
}

void Recorder::writeObservation(const TrackObservation& obs)
{
    write(withHead(formatIso8601Ms(obs.received_time), "type", "obs", recording::observationRecord(obs)));
}

void Recorder::writeUplink(std::chrono::system_clock::time_point when, const std::string& hex)
{
    write(withHead(formatIso8601Ms(when), "type", "uat_uplink", {recording::textField("uplink_hex", hex)}));
}

void Recorder::writeTrack(const skycue::Track& track, const char* event)
{
    if (!event || (std::strcmp(event, "update") != 0 && std::strcmp(event, "keyframe") != 0))
        throw std::invalid_argument(std::string("recorder: track event must be \"update\" or \"keyframe\", not \"") +
                                    (event ? event : "(null)") + "\"");
    write(withHead(now8601(), "event", event, recording::trackRecord(track)));
}

void Recorder::writeDrop(skycue::TrackId id, skycue::DropReason reason)
{
    write(withHead(now8601(), "event", "drop",
                   {recording::textField(recording::sentKey("hex"), hexAddress(id.address)),
                    recording::textField(recording::sentKey("addr_type"), addressTypeName(id.type)),
                    recording::textField(recording::sentKey("drop_reason"), dropReasonName(reason))}));
}

void Recorder::writeFilterChange(const std::string& description)
{
    write(withHead(now8601(), "event", "filter_change",
                   {recording::textField(recording::sentKey("note"), description)}));
}

void Recorder::flush()
{
    if (!open_)
        return;
    flushStreams();
    if (!streamsGood())
        fail("flushing " + stem_ + " failed: " + std::strerror(errno));
}

std::string Recorder::currentPath(const char* ext) const
{
    std::string e = ext ? ext : "";
    if (!e.empty() && e[0] == '.')
        e.erase(0, 1);
    if (!open_)
        return "";
    if (e == "jsonl" && config_.jsonl)
        return stem_ + ".jsonl";
    if (e == "csv" && config_.csv)
        return stem_ + ".csv";
    return "";
}

uint64_t Recorder::bytesWritten(const char* ext) const
{
    std::string e = ext ? ext : "";
    if (!e.empty() && e[0] == '.')
        e.erase(0, 1);
    if (e == "jsonl" && config_.jsonl)
        return jsonl_bytes_.load(std::memory_order_relaxed);
    if (e == "csv" && config_.csv)
        return csv_bytes_.load(std::memory_order_relaxed);
    return 0;
}

}  // namespace skycued
