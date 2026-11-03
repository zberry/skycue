// libFuzzer harness: one line of a recording, through the pure parsers (no
// socket, no file). Mode byte (fuzz_util.hpp splitMode):
//   0  parseRecordingLine (recording_reader.hpp), then for a line ReplaySource
//      plays or skycue-stats reads, replayDelay (replay_source.hpp) of its
//      time against a fixed start at several speeds
//   1  parseCaptureLine (asterix/capture_line.hpp); a line that parses must
//      come back unchanged through captureLineJson and parseCaptureLine
// Seeds: seeds/recording/ (docs/hardening.md "Fuzzing").

#include "asterix/capture_line.hpp"
#include "fuzz_util.hpp"
#include "recording_reader.hpp"
#include "replay_source.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {

void recordingLine(const std::string& line)
{
    skycued::RecordingRecord rec;
    const auto kind = skycued::parseRecordingLine(line, rec);
    if (kind != skycued::LineKind::received && kind != skycued::LineKind::sent &&
        kind != skycued::LineKind::readsb_raw) {
        return;
    }
    // observation.hpp: lat and lon are both set or neither.
    FUZZ_CHECK(rec.obs.lat.has_value() == rec.obs.lon.has_value());

    // A replay whose first record is at 2026-11-02T00:00:00Z.
    const std::chrono::system_clock::time_point start{std::chrono::seconds(1793577600)};
    const auto offset = rec.time - start;
    const auto max = std::chrono::duration<double>(skycued::kMaxReplayDelaySeconds);
    for (double speed : {1e-6, 0.5, 1.0, 64.0, 1e6}) {
        const auto d = skycued::replayDelay(offset, speed);
        FUZZ_CHECK(d.count() >= 0);
        FUZZ_CHECK(std::chrono::duration<double>(d) <= max);
    }
}

void captureLine(const std::string& text)
{
    const auto c = skycued::parseCaptureLine(text);
    if (!c) {
        return;
    }
    // capture_line.hpp: parseCaptureLine is the inverse of captureLineJson.
    const auto back = skycued::parseCaptureLine(skycued::captureLineJson(*c));
    FUZZ_CHECK(back.has_value());
    FUZZ_CHECK(back->input == c->input);
    FUZZ_CHECK(back->transport == c->transport);
    FUZZ_CHECK(back->bytes == c->bytes);
    FUZZ_CHECK(back->t == c->t);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    skycuefuzz::canary(data, size);
    skycuefuzz::quietLogs();
    const auto in = skycuefuzz::splitMode(data, size, 2);
    if (in.mode == 0) {
        recordingLine(in.payload);
    } else {
        captureLine(in.payload);
    }
    return 0;
}
