// libFuzzer harness: one line of readsb's JSON output port, through the pure
// parser parseReadsbLine (no socket, no file). Single mode: the whole input
// is the line. Seeds: seeds/readsb_line/ (docs/hardening.md "Fuzzing").

#include "fuzz_util.hpp"
#include "readsb_json_source.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    skycuefuzz::canary(data, size);
    skycuefuzz::quietLogs();
    const std::string line(reinterpret_cast<const char*>(data), size);
    const auto obs = skycued::parseReadsbLine(line);
    if (obs) {
        // The received recording stores the line as it came in.
        FUZZ_CHECK(obs->raw_line == line);
        // observation.hpp: lat and lon are both set or neither.
        FUZZ_CHECK(obs->lat.has_value() == obs->lon.has_value());
    }
    return 0;
}
