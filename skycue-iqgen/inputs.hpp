#pragma once

// Recorded frames for skycue-iqgen: Mode S from Beast (.bin), "epoch,HEX"
// (.csv) and AVR (.txt/.avr) captures, UAT from dump978 raw lines
// (.txt/.log); each optionally gzip-compressed (.gz after the suffix).

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace iqgen {

// A recorded frame. time: Mode S in 12 MHz ticks, UAT in ns; nullopt = untimed.
struct TimedFrame {
    std::optional<int64_t> time;
    std::vector<uint8_t> payload;
    std::string where;  // "FILE:LINE" or "FILE@offset"
};

struct InputStats {
    // Lines (or Beast frames) skipped: malformed Mode S lines, Mode A/C
    // lines and frames, lines of a UAT file that are not UAT (stratux "START,"
    // headers included) and stratux lines with an unusable payload. Blank
    // lines and '#' comments are not counted.
    uint64_t skipped = 0;
};

// An input the tool cannot use at all (unknown suffix, missing file, bad
// UAT payload length): a usage error, exit 2.
struct InputError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// .bin[.gz]: Beast types '2'/'3' only (payload = body after the 6-byte
//   timestamp and the signal byte); time = the 48-bit counter unwrapped by
//   accumulating beast::timestampDelta from the previous frame.
// .csv[.gz]: parseCsvMessage, time = epoch_ns * 3 / 250.
// .txt[.gz]/.avr[.gz]: parseAvrLine; "@" ticks unwrapped as for .bin, "*" untimed.
// Throws InputError for an unknown suffix or a file that cannot be opened;
// std::runtime_error for a read error (a truncated gzip stream).
std::vector<TimedFrame> readModesFile(const std::string& path, InputStats& stats);

// .txt[.gz]/.log[.gz]: parseUatLine; payload = the hex between the sign and
// the first ';'; time = epoch_ms * 1e6 when the line has one, offset_ns on a
// stratux "<ns>," line. A payload that is not 18, 34 or 432 bytes of hex
// throws InputError naming FILE:LINE, except on a stratux line: that line is
// skipped and counted.
std::vector<TimedFrame> readUatFile(const std::string& path, InputStats& stats);

}  // namespace iqgen
