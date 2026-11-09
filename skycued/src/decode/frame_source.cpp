#include "decode/frame_source.hpp"

#include "hex.hpp"

namespace skycued::decode {

namespace {

constexpr uint64_t kMlatMarker = 0xFF004D4C4154ULL;

}  // namespace

std::optional<RawFrame> fromBeast(const beast::Frame& f, FrameSourceCounters& c) {
    if (f.type == '1') {
        ++c.modeac_skipped;
        return std::nullopt;
    }
    if (f.type != '2' && f.type != '3' && f.type != '5') {
        ++c.other_skipped;
        return std::nullopt;
    }
    const std::size_t body_len = beast::bodyLength(f.type);  // 6 timestamp + 1 signal + payload
    const std::size_t payload = body_len - 7;
    if (f.timestamp == kMlatMarker) {
        ++c.mlat_skipped;
        return std::nullopt;
    }
    if (f.body.size() != body_len) {
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
        int hi = hexDigitValue(m->hex[i]), lo = hexDigitValue(m->hex[i + 1]);
        if (hi < 0 || lo < 0) {
            ++c.bad_avr;
            return std::nullopt;
        }
        r.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
    }
    return r;
}

}  // namespace skycued::decode
