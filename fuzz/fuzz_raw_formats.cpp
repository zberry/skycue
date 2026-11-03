// libFuzzer harness: the raw receiver formats of skycued/src/raw_formats.hpp
// (skycue-rawrec, skycue-rawplay), through their pure parsers (no socket, no
// file). Mode byte (fuzz_util.hpp splitMode):
//   0  parseUatLine     on the payload
//   1  parseAvrLine     on the payload
//   2  parseCsvMessage  on the payload
//   3  beast::FrameSplitter fed the payload in chunks (chunked(), seeded by
//      the payload's first 4 bytes)
// Seeds: seeds/raw_formats/ (docs/hardening.md "Fuzzing").

#include "fuzz_util.hpp"
#include "raw_formats.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool upperHex(const std::string& s)
{
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

void uat(const std::string& line)
{
    const auto u = skycued::parseUatLine(line);
    if (!u) {
        return;
    }
    // raw_formats.hpp: text starts with '-' or '+' and has no line end; the
    // prefix is an epoch or a stratux offset, never both.
    FUZZ_CHECK(!u->text.empty() && (u->text[0] == '-' || u->text[0] == '+'));
    FUZZ_CHECK(u->text.back() != '\n' && u->text.back() != '\r');
    FUZZ_CHECK(!(u->epoch_ms && u->offset_ns));
}

void avr(const std::string& line)
{
    const auto m = skycued::parseAvrLine(line);
    if (!m) {
        return;
    }
    FUZZ_CHECK(m->hex.size() == 14 || m->hex.size() == 28);
    FUZZ_CHECK(upperHex(m->hex));
    FUZZ_CHECK(!m->ticks || *m->ticks <= skycued::beast::kTimestampMask);
}

void csv(const std::string& line)
{
    const auto m = skycued::parseCsvMessage(line);
    if (!m) {
        return;
    }
    FUZZ_CHECK(m->hex.size() == 4 || m->hex.size() == 14 || m->hex.size() == 28);
    FUZZ_CHECK(upperHex(m->hex));
    FUZZ_CHECK(m->epoch_ns >= 0);
    // avrLine: ticks = (epoch_ns * 12 MHz) mod 2^48, read back by
    // parseAvrLine (which takes Mode S frames only).
    if (m->hex.size() != 4) {
        const auto back = skycued::parseAvrLine(skycued::avrLine(m->epoch_ns, m->hex));
        FUZZ_CHECK(back && back->ticks && back->hex == m->hex);
        const auto want = static_cast<uint64_t>(static_cast<unsigned __int128>(m->epoch_ns) * 3u / 250u) &
                          skycued::beast::kTimestampMask;
        FUZZ_CHECK(*back->ticks == want);
    }
}

void beastStream(const std::string& payload)
{
    uint32_t seed = 0;
    std::memcpy(&seed, payload.data(), payload.size() < 4 ? payload.size() : 4);
    skycued::beast::FrameSplitter s;
    std::vector<skycued::beast::Frame> frames;
    skycuefuzz::chunked(payload, seed, [&](const uint8_t* p, std::size_t n) {
        const std::size_t before = frames.size();
        s.feed(p, n, frames);
        for (std::size_t i = before; i < frames.size(); ++i) {
            const auto& f = frames[i];
            FUZZ_CHECK(f.body.size() == skycued::beast::bodyLength(f.type));
            FUZZ_CHECK(!f.raw.empty() && uint8_t(f.raw[0]) == 0x1a);
            FUZZ_CHECK(f.start + f.raw.size() <= s.fed());
        }
    });
    FUZZ_CHECK(s.skippedBytes() <= s.fed());
    FUZZ_CHECK(s.fed() == payload.size());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    skycuefuzz::canary(data, size);
    skycuefuzz::quietLogs();
    const auto in = skycuefuzz::splitMode(data, size, 4);
    switch (in.mode) {
    case 0: uat(in.payload); break;
    case 1: avr(in.payload); break;
    case 2: csv(in.payload); break;
    default: beastStream(in.payload); break;
    }
    return 0;
}
