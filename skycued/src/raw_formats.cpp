#include "raw_formats.hpp"

#include <cstdio>

namespace skycued {
namespace beast {

std::size_t bodyLength(uint8_t type)
{
    switch (type) {
    case '1': return 6 + 1 + 2;
    case '2': return 6 + 1 + 7;
    case '3':
    case '4':
    case '5': return 6 + 1 + 14;
    case 0xe3: return 8;
    default: return 0;
    }
}

void FrameSplitter::reset()
{
    state_ = State::hunt;
    pending_escape_ = false;
    cur_ = Frame{};
    need_ = 0;
    pos_ = 0;
}

void FrameSplitter::beginFrame(uint8_t type, uint64_t start)
{
    cur_ = Frame{};
    cur_.type = type;
    cur_.start = start;
    cur_.raw.push_back(static_cast<char>(kEscape));
    cur_.raw.push_back(static_cast<char>(type));
    need_ = bodyLength(type);
    pending_escape_ = false;
    state_ = State::body;
}

void FrameSplitter::feed(const uint8_t* data, std::size_t n, std::vector<Frame>& out)
{
    for (std::size_t i = 0; i < n; ++i, ++pos_) {
        const uint8_t c = data[i];
        switch (state_) {
        case State::hunt:
            if (c == kEscape) {
                esc_pos_ = pos_;
                state_ = State::type;
            } else {
                ++skipped_;
            }
            break;
        case State::type:
            if (bodyLength(c) > 0) {
                beginFrame(c, esc_pos_);
            } else if (c == kEscape) {
                // A doubled 0x1a outside any frame (we joined in the middle of
                // one), or a lone 0x1a left by a frame cut short right after
                // its first byte: skip the first and let this one start a frame.
                ++skipped_;
                esc_pos_ = pos_;
            } else {
                skipped_ += 2;  // an unknown type
                state_ = State::hunt;
            }
            break;
        case State::body:
            if (pending_escape_) {
                pending_escape_ = false;
                if (c == kEscape) {
                    cur_.raw.push_back(static_cast<char>(c));
                    cur_.body.push_back(static_cast<char>(c));
                } else {
                    // A lone 0x1a: the frame was cut short and a new one starts there.
                    skipped_ += cur_.raw.size() - 1;  // all but that 0x1a
                    if (bodyLength(c) > 0) {
                        beginFrame(c, pos_ - 1);
                    } else {
                        skipped_ += 2;
                        state_ = State::hunt;
                    }
                    break;
                }
            } else {
                cur_.raw.push_back(static_cast<char>(c));
                if (c == kEscape) {
                    pending_escape_ = true;
                    break;
                }
                cur_.body.push_back(static_cast<char>(c));
            }
            if (cur_.body.size() == need_) {
                if (cur_.type != 0xe3)
                    for (int k = 0; k < 6; ++k)
                        cur_.timestamp = (cur_.timestamp << 8) | static_cast<uint8_t>(cur_.body[static_cast<std::size_t>(k)]);
                out.push_back(std::move(cur_));
                cur_ = Frame{};
                state_ = State::hunt;
            }
            break;
        }
    }
}

std::string withTimestamp(const Frame& frame, uint64_t timestamp)
{
    if (frame.type == 0xe3 || frame.body.size() < 6)
        return frame.raw;
    std::string body = frame.body;
    timestamp &= kTimestampMask;
    for (int k = 0; k < 6; ++k)
        body[static_cast<std::size_t>(k)] = static_cast<char>((timestamp >> (8 * (5 - k))) & 0xff);
    std::string raw;
    raw.reserve(2 + 2 * body.size());
    raw.push_back(static_cast<char>(kEscape));
    raw.push_back(static_cast<char>(frame.type));
    for (char c : body) {
        raw.push_back(c);
        if (static_cast<uint8_t>(c) == kEscape)
            raw.push_back(c);
    }
    return raw;
}

int64_t timestampDelta(uint64_t from, uint64_t to)
{
    const uint64_t d = (to - from) & kTimestampMask;
    const uint64_t half = uint64_t{1} << 47;
    return d >= half ? static_cast<int64_t>(d) - static_cast<int64_t>(uint64_t{1} << 48) : static_cast<int64_t>(d);
}

}  // namespace beast

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::string chomp(const std::string& line)
{
    std::size_t end = line.size();
    while (end > 0 && (line[end - 1] == '\n' || line[end - 1] == '\r'))
        --end;
    return line.substr(0, end);
}

}  // namespace

std::optional<CsvMessage> parseCsvMessage(const std::string& raw)
{
    const std::string line = chomp(raw);
    const std::size_t comma = line.find(',');
    if (comma == std::string::npos || comma == 0)
        return std::nullopt;
    // seconds[.fraction], at most 10 integer digits and at most kMaxSecs, so
    // secs * 1e9 + fraction stays inside int64_t (epochs until 2262).
    constexpr int64_t kMaxSecs = 9223372035;
    int64_t secs = 0, frac_ns = 0;
    std::size_t i = 0, int_digits = 0;
    for (; i < comma && isDigit(line[i]); ++i, ++int_digits) {
        if (int_digits == 10)
            return std::nullopt;  // checked before the multiply, which would overflow
        secs = secs * 10 + (line[i] - '0');
    }
    if (int_digits == 0 || secs > kMaxSecs)
        return std::nullopt;
    if (i < comma) {
        if (line[i] != '.')
            return std::nullopt;
        ++i;
        int64_t scale = 100000000;  // first fraction digit = 1e8 ns
        for (; i < comma; ++i) {
            if (!isDigit(line[i]))
                return std::nullopt;
            frac_ns += (line[i] - '0') * scale;
            scale /= 10;
        }
    }
    CsvMessage m;
    m.epoch_ns = secs * 1000000000LL + frac_ns;
    const std::size_t n = line.size() - comma - 1;
    if (n != 4 && n != 14 && n != 28)
        return std::nullopt;
    m.hex.reserve(n);
    for (std::size_t k = comma + 1; k < line.size(); ++k) {
        const int v = hexValue(line[k]);
        if (v < 0)
            return std::nullopt;
        m.hex.push_back("0123456789ABCDEF"[v]);
    }
    return m;
}

std::string avrLine(int64_t epoch_ns, const std::string& hex)
{
    // ns * 12e6 / 1e9 = ns * 3 / 250, as 3 * (ns / 250) + 3 * (ns % 250) / 250
    // so the product never wraps (ns * 3 would past 2^64 / 3, the year 2165).
    const uint64_t ns = static_cast<uint64_t>(epoch_ns);
    const uint64_t ticks = (ns / 250u * 3u + ns % 250u * 3u / 250u) & beast::kTimestampMask;
    char ts[16];
    std::snprintf(ts, sizeof ts, "%012llX", static_cast<unsigned long long>(ticks));
    std::string out;
    out.reserve(1 + 12 + hex.size() + 1);
    out += '@';
    out += ts;
    out += hex;
    out += ';';
    return out;
}

std::optional<AvrMessage> parseAvrLine(const std::string& raw)
{
    const std::string line = chomp(raw);
    if (line.size() < 2 || (line[0] != '@' && line[0] != '*') || line.back() != ';')
        return std::nullopt;
    const bool timed = line[0] == '@';
    const std::size_t digits = line.size() - 2;
    const std::size_t n = digits - (timed ? 12 : 0);
    if ((timed && digits < 12) || (n != 14 && n != 28))
        return std::nullopt;
    AvrMessage m;
    uint64_t ticks = 0;
    m.hex.reserve(n);
    for (std::size_t i = 1; i + 1 < line.size(); ++i) {
        const int v = hexValue(line[i]);
        if (v < 0)
            return std::nullopt;
        if (timed && i <= 12)
            ticks = (ticks << 4) | static_cast<uint64_t>(v);
        else
            m.hex += static_cast<char>(v < 10 ? '0' + v : 'A' + v - 10);
    }
    if (timed)
        m.ticks = ticks;
    return m;
}

std::optional<UatLine> parseUatLine(const std::string& raw)
{
    std::string line = chomp(raw);
    UatLine u;
    if (!line.empty() && isDigit(line[0])) {
        // At most 19 digits: below 10^19 < 2^64, so v cannot wrap.
        std::size_t i = 0;
        uint64_t v = 0;
        for (; i < line.size() && isDigit(line[i]) && i < 19; ++i)
            v = v * 10 + static_cast<uint64_t>(line[i] - '0');
        if (i < line.size() && line[i] == ' ' && i <= 18)
            u.epoch_ms = static_cast<int64_t>(v);  // "<epoch_ms> " (skycue-rawrec)
        else if (i < line.size() && line[i] == ',' && v <= static_cast<uint64_t>(INT64_MAX))
            u.offset_ns = static_cast<int64_t>(v);  // "<ns>," (stratux)
        else
            return std::nullopt;
        line.erase(0, i + 1);
    }
    if (line.empty() || (line[0] != '-' && line[0] != '+'))
        return std::nullopt;
    u.text = std::move(line);
    return u;
}

bool isStratuxStart(const std::string& line)
{
    return line.compare(0, 6, "START,") == 0;
}

std::optional<int64_t> StratuxClock::next(int64_t offset_ns)
{
    int64_t first = offset_ns, start = offset_ns;
    if (have_) {
        if (restart_ || offset_ns < last_off_) {
            if (last_ > INT64_MAX - kGapNs)
                return std::nullopt;
            start = last_ + kGapNs;
        } else {
            first = first_;
            start = start_;
        }
    }
    const int64_t rel = offset_ns - first;  // >= 0: offsets are never negative
    if (rel > INT64_MAX - start)
        return std::nullopt;
    have_ = true;
    restart_ = false;
    first_ = first;
    start_ = start;
    last_off_ = offset_ns;
    last_ = start + rel;
    return last_;
}

}  // namespace skycued
