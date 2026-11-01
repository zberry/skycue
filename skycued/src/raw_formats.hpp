#pragma once

// Raw receiver formats shared by skycue-rawrec and skycue-rawplay: the Beast
// binary stream (readsb's Beast output/input), AVR "@" lines (readsb's raw
// input) synthesized from "epoch,HEX" captures, and dump978 raw UAT lines.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skycued {
namespace beast {

constexpr uint8_t kEscape = 0x1a;
constexpr uint64_t kTimestampMask = (uint64_t{1} << 48) - 1;
constexpr double kTicksPerSecond = 12e6;  // the 12 MHz Beast clock

// Bytes after the type byte (before unescaping) of a frame of this type:
// 6-byte timestamp + 1-byte signal + payload for '1' (Mode A/C, 2 bytes),
// '2' (short Mode S, 7), '3' (long Mode S, 14), '4' (Radarcape status, 14)
// and '5' (readsb: long Mode S variant, 14); 8 for 0xe3 (receiver id, which
// prefixes the next frame). 0 for every other type byte.
std::size_t bodyLength(uint8_t type);

// One frame as found in the stream.
struct Frame {
    uint8_t type = 0;
    uint64_t timestamp = 0;  // 48 bits; 0 for 0xe3
    std::string raw;         // exactly the bytes on the wire: 0x1a, type, escaped body
    std::string body;        // unescaped body (bodyLength(type) bytes)
    uint64_t start = 0;      // stream offset of the leading 0x1a (bytes fed since reset)
};

// Incremental Beast frame parser. Bytes outside frames, frames of unknown
// types, and frames cut short by an unescaped 0x1a (a new frame start, as
// readsb treats it) are skipped and counted.
class FrameSplitter {
public:
    // Appends every frame completed by these bytes to `out`.
    void feed(const uint8_t* data, std::size_t n, std::vector<Frame>& out);
    void reset();                                       // forget any partial frame; offsets restart at 0
    uint64_t skippedBytes() const { return skipped_; }  // not part of any returned frame
    uint64_t fed() const { return pos_; }

private:
    enum class State { hunt, type, body };
    void beginFrame(uint8_t type, uint64_t start);
    State state_ = State::hunt;
    bool pending_escape_ = false;  // in body: the last byte was a lone 0x1a
    Frame cur_;
    std::size_t need_ = 0;
    uint64_t esc_pos_ = 0;  // offset of the 0x1a that may start a frame
    uint64_t pos_ = 0;
    uint64_t skipped_ = 0;
};

// The frame's bytes with its timestamp replaced (escapes redone). Frames
// without a timestamp (0xe3) come back unchanged.
std::string withTimestamp(const Frame& frame, uint64_t timestamp);

// to - from as a signed 48-bit difference, so a counter wrap reads as a small
// positive step.
int64_t timestampDelta(uint64_t from, uint64_t to);

}  // namespace beast

// One "epoch_seconds.fraction,HEX" capture line.
struct CsvMessage {
    int64_t epoch_ns = 0;
    std::string hex;  // uppercase
};
// nullopt for blank, comment, malformed lines and HEX that is not 4, 14 or
// 28 hex digits. Never throws.
std::optional<CsvMessage> parseCsvMessage(const std::string& line);
// "@TTTTTTTTTTTTHEX;" (no newline): TT... = (epoch_ns * 12 MHz) mod 2^48 as
// 12 uppercase hex digits, the AVR-with-timestamp form readsb's raw input reads.
std::string avrLine(int64_t epoch_ns, const std::string& hex);

// A dump978 raw line, optionally "<epoch_ms> "-prefixed as skycue-rawrec writes it.
struct UatLine {
    std::optional<int64_t> epoch_ms;
    std::string text;  // starts with '-' (downlink) or '+' (uplink); no line end
};
// nullopt for anything else (blank lines, comments, ...). Trailing CR/LF dropped.
std::optional<UatLine> parseUatLine(const std::string& line);

}  // namespace skycued
