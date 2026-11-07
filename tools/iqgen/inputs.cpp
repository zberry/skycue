#include "inputs.hpp"

#include "coding/bits.hpp"
#include "gz_line_reader.hpp"
#include "raw_formats.hpp"
#include "uat_frame.hpp"

#include <cstdint>
#include <memory>

namespace iqgen {

namespace {

bool endsWith(const std::string& s, const std::string& tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// The suffix that names the format: ".csv" for "x.csv" and "x.csv.gz".
std::string formatSuffix(const std::string& path)
{
    std::string p = path;
    if (endsWith(p, ".gz"))
        p.erase(p.size() - 3);
    const std::size_t slash = p.find_last_of('/');
    const std::size_t dot = p.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return "";
    return p.substr(dot);
}

std::unique_ptr<skycued::GzLineReader> openInput(const std::string& path)
{
    try {
        return std::make_unique<skycued::GzLineReader>(path);
    } catch (const std::exception& e) {
        throw InputError(e.what());
    }
}

std::string chomp(std::string line)
{
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    return line;
}

bool ignorable(const std::string& line)
{
    return line.empty() || line[0] == '#';
}

std::vector<uint8_t> hexBytes(const std::string& hex)
{
    std::vector<uint8_t> out;
    skycued::coding::parseHex(hex, out);  // the parsers only hand on valid hex
    return out;
}

// Accumulates 48-bit counter values into an unwrapped time. Throws
// InputError (naming where) if the sum would leave int64_t, which takes
// 2^15 or more consecutive near-maximal steps in one direction.
class Unwrapper {
public:
    int64_t next(uint64_t counter, const std::string& where)
    {
        if (have_) {
            const int64_t d = skycued::beast::timestampDelta(last_, counter);
            if ((d > 0 && time_ > INT64_MAX - d) || (d < 0 && time_ < INT64_MIN - d))
                throw InputError(where + ": timestamps run out of range (the 48-bit counter unwraps past 2^63)");
            time_ += d;
        } else
            time_ = static_cast<int64_t>(counter);
        have_ = true;
        last_ = counter;
        return time_;
    }

private:
    bool have_ = false;
    uint64_t last_ = 0;
    int64_t time_ = 0;
};

std::vector<TimedFrame> readBeast(const std::string& path, InputStats& stats)
{
    auto in = openInput(path);
    std::vector<TimedFrame> out;
    skycued::beast::FrameSplitter splitter;
    std::vector<skycued::beast::Frame> frames;
    Unwrapper clock;
    std::vector<uint8_t> buf(256 * 1024);
    for (std::size_t n; (n = in->read(buf.data(), buf.size())) > 0;) {
        frames.clear();
        splitter.feed(buf.data(), n, frames);
        for (const auto& f : frames) {
            if (f.type != '2' && f.type != '3') {
                if (f.type != 0xe3)
                    ++stats.skipped;
                continue;
            }
            TimedFrame t;
            t.where = path + "@" + std::to_string(f.start);
            t.time = clock.next(f.timestamp, t.where);
            t.payload.assign(f.body.begin() + 7, f.body.end());
            out.push_back(std::move(t));
        }
    }
    return out;
}

std::vector<TimedFrame> readCsv(const std::string& path, InputStats& stats)
{
    auto in = openInput(path);
    std::vector<TimedFrame> out;
    std::string line;
    for (uint64_t no = 1; in->getline(line); ++no) {
        line = chomp(line);
        if (ignorable(line))
            continue;
        const auto m = skycued::parseCsvMessage(line);
        if (!m || m->hex.size() == 4) {
            ++stats.skipped;
            continue;
        }
        // ns * 12e6 / 1e9; the parser keeps epoch_ns below 2^63 / 3.
        out.push_back({m->epoch_ns * 3 / 250, hexBytes(m->hex), path + ":" + std::to_string(no)});
    }
    return out;
}

std::vector<TimedFrame> readAvr(const std::string& path, InputStats& stats)
{
    auto in = openInput(path);
    std::vector<TimedFrame> out;
    Unwrapper clock;
    std::string line;
    for (uint64_t no = 1; in->getline(line); ++no) {
        line = chomp(line);
        if (ignorable(line))
            continue;
        const auto m = skycued::parseAvrLine(line);
        if (!m) {
            ++stats.skipped;
            continue;
        }
        TimedFrame t;
        t.where = path + ":" + std::to_string(no);
        if (m->ticks)
            t.time = clock.next(*m->ticks, t.where);
        t.payload = hexBytes(m->hex);
        out.push_back(std::move(t));
    }
    return out;
}

}  // namespace

std::vector<TimedFrame> readModesFile(const std::string& path, InputStats& stats)
{
    const std::string suffix = formatSuffix(path);
    if (suffix == ".bin")
        return readBeast(path, stats);
    if (suffix == ".csv")
        return readCsv(path, stats);
    if (suffix == ".txt" || suffix == ".avr")
        return readAvr(path, stats);
    throw InputError(path + ": unknown Mode S input (want .bin, .csv, .txt or .avr, each optionally .gz)");
}

std::vector<TimedFrame> readUatFile(const std::string& path, InputStats& stats)
{
    const std::string suffix = formatSuffix(path);
    if (suffix != ".txt" && suffix != ".log")
        throw InputError(path + ": unknown UAT input (want .txt or .log, each optionally .gz)");
    auto in = openInput(path);
    std::vector<TimedFrame> out;
    skycued::StratuxClock stratux;
    std::string line;
    for (uint64_t no = 1; in->getline(line); ++no) {
        line = chomp(line);
        if (ignorable(line))
            continue;
        const auto u = skycued::parseUatLine(line);
        if (!u) {
            if (skycued::isStratuxStart(line))
                stratux.startSession();
            ++stats.skipped;
            continue;
        }
        const std::string where = path + ":" + std::to_string(no);
        const std::size_t semi = u->text.find(';');
        const std::string hex = u->text.substr(1, semi == std::string::npos ? std::string::npos : semi - 1);
        TimedFrame t;
        if (!skycued::coding::parseHex(hex, t.payload) || !uatKindForPayload(t.payload.size())) {
            // A stratux log keeps whatever the radio handed on; skip such lines there.
            if (u->offset_ns) {
                ++stats.skipped;
                continue;
            }
            throw InputError(where + ": UAT payload is not 18, 34 or 432 bytes of hex (" +
                             std::to_string(hex.size()) + " characters)");
        }
        if (u->offset_ns) {
            t.time = stratux.next(*u->offset_ns);
            if (!t.time)
                throw InputError(where + ": timestamp out of range");
        } else if (u->epoch_ms) {
            if (*u->epoch_ms > INT64_MAX / 1000000)
                throw InputError(where + ": timestamp out of range");
            t.time = *u->epoch_ms * 1000000;
        }
        t.where = where;
        out.push_back(std::move(t));
    }
    return out;
}

}  // namespace iqgen
