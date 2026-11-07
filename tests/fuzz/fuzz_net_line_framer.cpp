// libFuzzer harness: skycue::net::LineFramer fed with the input cut into
// chunks of pseudo-random sizes derived from the data itself. Every emitted
// message must be non-empty, free of line breaks and within the limit, and the
// rejection count must never go down. Pure in-memory, no socket.

#include "skycue/net/framer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define NET_FUZZ_CHECK(cond)                                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    constexpr std::size_t kMax = 256;
    skycue::net::LineFramer framer(kMax);
    uint64_t last_rejected = 0;
    const auto out = [](const uint8_t* msg, std::size_t n) {
        NET_FUZZ_CHECK(n > 0 && n <= kMax);
        for (std::size_t i = 0; i < n; ++i)
            NET_FUZZ_CHECK(msg[i] != '\n' && msg[i] != '\r');
    };

    uint32_t state = 2166136261u; // chunk sizes from an FNV-style hash of the input
    std::size_t pos = 0;
    while (pos < size) {
        state = (state ^ data[pos]) * 16777619u;
        const std::size_t chunk = std::min<std::size_t>(1 + (state >> 24) % 97, size - pos);
        framer.feed(data + pos, chunk, out);
        pos += chunk;
        NET_FUZZ_CHECK(framer.rejected() >= last_rejected);
        last_rejected = framer.rejected();
    }
    framer.reset();
    return 0;
}
