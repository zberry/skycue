// libFuzzer harness: the 1090 MHz decoder (ModesDecoder). The input is cut
// into frames: each frame is a length byte (odd: 14 bytes, even: 7 bytes)
// followed by that many bytes (the rest of the input if short); every frame
// is fed with a rising arrival time. The decoder must not throw and every
// frame must be counted. Seeds: seeds/decode_1090/.

#include "decode/modes_decoder.hpp"
#include "fuzz_util.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    skycuefuzz::canary(data, size);
    skycuefuzz::quietLogs();
    skycued::decode::DecoderConfig cfg;
    cfg.max_keys = 64;  // small, so eviction is exercised
    skycued::decode::ModesDecoder dec(cfg, "fuzz");
    dec.onObservation([](const skycued::TrackObservation& o) {
        // observation.hpp: lat and lon are both set or neither.
        FUZZ_CHECK(o.lat.has_value() == o.lon.has_value());
    });
    using Clock = skycued::decode::ModesDecoder::Clock;
    const auto t0 = Clock::time_point(std::chrono::seconds(1700000000));
    std::size_t pos = 0;
    uint64_t frames = 0;
    while (pos < size) {
        const std::size_t want = (data[pos] & 1u) ? 14u : 7u;
        ++pos;
        const std::size_t n = want < size - pos ? want : size - pos;
        const std::vector<uint8_t> frame(data + pos, data + pos + n);
        pos += n;
        dec.feed(frame, t0 + std::chrono::milliseconds(frames * 250), std::nullopt);
        ++frames;
    }
    FUZZ_CHECK(dec.counters().frames == frames);
    FUZZ_CHECK(dec.keys() <= 64);
    return 0;
}
