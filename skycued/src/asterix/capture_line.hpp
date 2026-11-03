#pragma once

// One line of a pingStation capture file (spec §5.4): a datagram (UDP) or a
// framed CAT021 block (TCP) as received, with its receive time, input id
// and transport. Lives in skycued_asterix (PropertyTree only, no skycued_core)
// so skycue-dump and skycue-pingsim can read capture files.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skycued {

struct CaptureLine {
    std::chrono::system_clock::time_point t;  // receive time
    std::string input, transport;             // transport: "udp", "tcp_connect" or "tcp_listen"
    std::vector<uint8_t> bytes;
};

// {"t":"<formatIso8601Ms>","input":"<id>","transport":"<transport>","hex":"<lowercase>"}
// with no newline.
std::string captureLineJson(const CaptureLine& line);

// The inverse; also accepts uppercase hex, any key order and a trailing line
// end. nullopt for anything malformed: not a JSON object, a key missing or
// not a string, a bad time, an empty input or one that is not valid UTF-8,
// an unknown transport, hex of odd length or with a non-hex character. Never
// throws.
std::optional<CaptureLine> parseCaptureLine(const std::string& text);

}  // namespace skycued
