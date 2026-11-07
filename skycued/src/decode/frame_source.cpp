#include "decode/frame_source.hpp"

namespace skycued::decode {

namespace {

constexpr uint64_t kMlatMarker = 0xFF004D4C4154ULL;

int hexVal(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

}  // namespace

std::optional<RawFrame> fromBeast(const beast::Frame& f, FrameSourceCounters& c) {
    std::size_t payload = 0;
    switch (f.type) {
        case '2': payload = 7; break;
        case '3':
        case '5': payload = 14; break;
        case '1': ++c.modeac_skipped; return std::nullopt;
        default: ++c.other_skipped; return std::nullopt;
    }
    if (f.timestamp == kMlatMarker) {
        ++c.mlat_skipped;
        return std::nullopt;
    }
    if (f.body.size() != 7 + payload) {
        ++c.other_skipped;
        return std::nullopt;
    }
    RawFrame r;
    r.signal = static_cast<uint8_t>(f.body[6]);
    r.bytes.reserve(payload);
    for (std::size_t i = 0; i < payload; ++i) r.bytes.push_back(static_cast<uint8_t>(f.body[7 + i]));
    return r;
}

std::optional<RawFrame> fromAvrLine(const std::string& line, FrameSourceCounters& c) {
    auto m = parseAvrLine(line);
    if (!m || (m->hex.size() != 14 && m->hex.size() != 28)) {
        ++c.bad_avr;
        return std::nullopt;
    }
    if (m->ticks && *m->ticks == kMlatMarker) {
        ++c.mlat_skipped;
        return std::nullopt;
    }
    RawFrame r;
    r.bytes.reserve(m->hex.size() / 2);
    for (std::size_t i = 0; i < m->hex.size(); i += 2) {
        int hi = hexVal(m->hex[i]), lo = hexVal(m->hex[i + 1]);
        if (hi < 0 || lo < 0) {
            ++c.bad_avr;
            return std::nullopt;
        }
        r.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
    }
    return r;
}

}  // namespace skycued::decode
