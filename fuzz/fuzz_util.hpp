#pragma once

// Shared helpers for the libFuzzer harnesses in skycue/fuzz (docs/hardening.md
// "Fuzzing"). Header-only; every harness includes it.

#include "log.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

// FUZZ_CHECK(cond): an invariant of the code under test. On false it names
// the condition and aborts, which libFuzzer records as a crash artefact.
#define FUZZ_CHECK(cond)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FUZZ_CHECK failed: %s at %s:%d\n", #cond,    \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

namespace skycuefuzz {

// A harness with several modes takes the first input byte as the mode
// ('0' + mode in the seeds, so a seed file reads "0{...}") and the rest as
// the payload.
struct Split {
    unsigned mode;
    std::string payload;
};

inline Split splitMode(const uint8_t* data, std::size_t size, unsigned modes)
{
    if (size == 0 || modes == 0) {
        return Split{0, std::string()};
    }
    const unsigned mode = static_cast<uint8_t>(data[0] - '0') % modes;
    return Split{mode, std::string(reinterpret_cast<const char*>(data) + 1, size - 1)};
}

// Feeds payload to feed in chunks of 1..64 bytes, the sizes drawn from a
// xorshift32 seeded with seed | 1, so a stream parser sees its input split at
// arbitrary points and the same input always splits the same way.
inline void chunked(const std::string& payload, uint32_t seed,
                    const std::function<void(const uint8_t*, std::size_t)>& feed)
{
    uint32_t x = seed | 1u;
    std::size_t pos = 0;
    while (pos < payload.size()) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        std::size_t n = 1 + x % 64u;
        if (n > payload.size() - pos) {
            n = payload.size() - pos;
        }
        feed(reinterpret_cast<const uint8_t*>(payload.data()) + pos, n);
        pos += n;
    }
}

// True when s is well-formed UTF-8 (RFC 3629): no overlong forms, no
// surrogates (U+D800..U+DFFF), nothing above U+10FFFF.
inline bool validUtf8(const std::string& s)
{
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t len;
        unsigned char lo = 0x80, hi = 0xbf;  // bounds of the second byte
        if (c >= 0xc2 && c <= 0xdf) {
            len = 2;
        } else if (c >= 0xe0 && c <= 0xef) {
            len = 3;
            if (c == 0xe0) lo = 0xa0;       // overlong
            else if (c == 0xed) hi = 0x9f;  // surrogates
        } else if (c >= 0xf0 && c <= 0xf4) {
            len = 4;
            if (c == 0xf0) lo = 0x90;       // overlong
            else if (c == 0xf4) hi = 0x8f;  // above U+10FFFF
        } else {
            return false;  // continuation byte, 0xc0/0xc1, or 0xf5..0xff
        }
        if (n - i < len) {
            return false;
        }
        if (p[i + 1] < lo || p[i + 1] > hi) {
            return false;
        }
        for (std::size_t k = 2; k < len; ++k) {
            if (p[i + k] < 0x80 || p[i + k] > 0xbf) {
                return false;
            }
        }
        i += len;
    }
    return true;
}

// Raises the daemon's log level to error once, so a parser's debug and warn
// lines do not slow the fuzzer down or bury its output.
inline void quietLogs()
{
    static const bool done = [] {
        skycued::setLogLevel(skycued::LogLevel::error);
        return true;
    }();
    (void)done;
}

// scripts/run-fuzz.sh --canary: with SKYCUE_FUZZ_CANARY=1 an input starting
// "skycue-canary" aborts, proving that a crash is turned into a finding.
inline void canary(const uint8_t* data, std::size_t size)
{
    static const bool armed = [] {
        const char* v = std::getenv("SKYCUE_FUZZ_CANARY");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    static const char kPrefix[] = "skycue-canary";
    const std::size_t len = sizeof(kPrefix) - 1;
    if (armed && size >= len && std::memcmp(data, kPrefix, len) == 0) {
        std::fprintf(stderr, "skycue fuzz canary: aborting on purpose\n");
        std::abort();
    }
}

}  // namespace skycuefuzz
