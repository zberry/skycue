// libFuzzer harness: the pingStation 3 inputs through their pure parsers (no
// socket, no file). Mode byte (fuzz_util.hpp splitMode), then the payload:
//   0  parsePingDatagram (inputs/ping_json.hpp), arrival 2026-11-02T10:15:00Z,
//      UTC time mode
//   1  parsePingStatusDocument, the same arrival
//   2  asterix::splitBlocks on the payload after its first byte, and
//      decodeBlock on every CAT 21 span, edition = first byte % 6
//   3  a Cat021StreamFramer fed the payload after its first byte in chunks
//      (chunked(), seeded by the next 4 bytes); edition = (first byte & 0x7f)
//      % 6, accept_other_categories = first byte & 0x80
// Seeds: seeds/pingstation/ (docs/hardening.md "Fuzzing").

#include "asterix/cat021.hpp"
#include "asterix/cat021_framer.hpp"
#include "fuzz_util.hpp"
#include "inputs/ping_json.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace ax = skycued::asterix;

// 2026-11-02T10:15:00Z.
const std::chrono::system_clock::time_point kArrival{std::chrono::seconds(1793614500)};

bool lowerHex(const std::string& s)
{
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

// "major.minor.build", each part an integer (digits, an optional leading '-').
bool versionText(const std::string& v)
{
    int parts = 1;
    bool digit = false, start = true;
    for (char c : v) {
        if (c >= '0' && c <= '9') {
            digit = true;
            start = false;
        } else if (c == '-' && start) {
            start = false;
        } else if (c == '.' && digit) {
            ++parts;
            digit = false;
            start = true;
        } else {
            return false;
        }
    }
    return parts == 3 && digit;
}

void checkStatus(const skycued::DeviceStatus& s)
{
    FUZZ_CHECK(s.version.empty() || versionText(s.version));
}

void datagram(const std::string& payload)
{
    skycued::PingContext ctx;
    ctx.arrival = kArrival;
    ctx.time_mode = skycued::TimeMode::utc;
    const skycued::PingResult r = skycued::parsePingDatagram(payload, ctx);
    for (const skycued::RawFrame& f : r.raw_frames) {
        FUZZ_CHECK(lowerHex(f.hex));
        FUZZ_CHECK(f.hex.size() % 2 == 0);
        const std::size_t bytes = f.hex.size() / 2;
        if (f.kind == skycued::RawFrame::Kind::adsb_1090)
            FUZZ_CHECK(bytes == 7 || bytes == 14);
        else
            FUZZ_CHECK(bytes == 18 || bytes == 34);
    }
    for (std::size_t i = 0; i < r.observations.size(); ++i) {
        const auto& o = r.observations[i];
        FUZZ_CHECK(o.lat.has_value() == o.lon.has_value());
        FUZZ_CHECK(i == 0 ? o.raw_line == payload : o.raw_line.empty());
    }
    if (r.status)
        checkStatus(*r.status);
}

void statusDocument(const std::string& payload)
{
    const auto s = skycued::parsePingStatusDocument(payload, kArrival);
    if (s)
        checkStatus(*s);
}

ax::Edition editionOf(uint8_t b)
{
    return static_cast<ax::Edition>(b % 6);
}

void split(const std::string& payload)
{
    if (payload.empty())
        return;
    const ax::Edition edition = editionOf(static_cast<uint8_t>(payload[0]));
    const auto* p = reinterpret_cast<const uint8_t*>(payload.data()) + 1;
    const std::size_t n = payload.size() - 1;
    const ax::BlockSplit s = ax::splitBlocks(p, n);
    std::size_t end = 0;  // spans are in order and do not overlap
    for (const ax::BlockSpan& b : s.blocks) {
        FUZZ_CHECK(b.offset >= end);
        FUZZ_CHECK(b.size >= 3);
        FUZZ_CHECK(b.offset <= n && b.size <= n - b.offset);
        FUZZ_CHECK(p[b.offset] == b.cat);
        FUZZ_CHECK(((std::size_t{p[b.offset + 1]} << 8) | p[b.offset + 2]) == b.size);
        end = b.offset + b.size;
        if (b.cat == 21)
            (void)ax::decodeBlock(p + b.offset, b.size, edition);
    }
}

void framer(const std::string& payload)
{
    if (payload.empty())
        return;
    const uint8_t first = static_cast<uint8_t>(payload[0]);
    const ax::Edition edition = editionOf(first & 0x7f);
    const bool other = (first & 0x80) != 0;
    const std::string stream = payload.substr(1);
    uint32_t seed = 0;
    std::memcpy(&seed, stream.data(), stream.size() < 4 ? stream.size() : 4);

    ax::Cat021StreamFramer f(edition, other);
    std::vector<std::vector<uint8_t>> out;
    skycuefuzz::chunked(stream, seed, [&](const uint8_t* d, std::size_t n) {
        const std::size_t before = out.size();
        f.feed(d, n, out);
        FUZZ_CHECK(f.buffered() <= 65535u);
        for (std::size_t i = before; i < out.size(); ++i) {
            const auto& b = out[i];
            FUZZ_CHECK(b.size() >= 3);
            FUZZ_CHECK(((std::size_t{b[1]} << 8) | b[2]) == b.size());
            FUZZ_CHECK(other || b[0] == 21);
            if (b[0] == 21) {
                // A block passed out walked cleanly (cat021_framer.hpp), so
                // the decoder agrees with it.
                FUZZ_CHECK(!ax::decodeBlock(b.data(), b.size(), edition).error);
            }
        }
    });
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    skycuefuzz::canary(data, size);
    skycuefuzz::quietLogs();
    const auto in = skycuefuzz::splitMode(data, size, 4);
    switch (in.mode) {
    case 0:
        datagram(in.payload);
        break;
    case 1:
        statusDocument(in.payload);
        break;
    case 2:
        split(in.payload);
        break;
    default:
        framer(in.payload);
        break;
    }
    return 0;
}
