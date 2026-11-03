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
// nullopt for blank, comment, malformed lines, seconds past 9223372035 (the
// int64_t nanosecond range) and HEX that is not 4, 14 or 28 hex digits.
// Never throws.
std::optional<CsvMessage> parseCsvMessage(const std::string& line);
// "@TTTTTTTTTTTTHEX;" (no newline): TT... = (epoch_ns * 12 MHz) mod 2^48 as
// 12 uppercase hex digits, the AVR-with-timestamp form readsb's raw input reads.
std::string avrLine(int64_t epoch_ns, const std::string& hex);

// One AVR line as readsb's raw output writes it.
struct AvrMessage {
    std::optional<uint64_t> ticks;  // the 48-bit 12 MHz counter of an "@" line; nullopt for "*"
    std::string hex;                // uppercase, 14 or 28 digits
};
// "@<12 hex ticks><HEX>;" or "*<HEX>;" with HEX 14 or 28 hex digits (trailing
// CR/LF ok); nullopt for anything else, Mode A/C (4 digits) included. The
// inverse of avrLine(). Never throws.
std::optional<AvrMessage> parseAvrLine(const std::string& line);

// A dump978 raw line, optionally "<epoch_ms> "-prefixed as skycue-rawrec writes
// it, or "<ns>,"-prefixed as stratux's UAT logs write it (a nanosecond offset
// from an arbitrary start, not an epoch).
struct UatLine {
    std::optional<int64_t> epoch_ms;
    std::optional<int64_t> offset_ns;
    std::string text;  // starts with '-' (downlink) or '+' (uplink); no line end
};
// nullopt for anything else (blank lines, comments, stratux "START,..." lines,
// an <ns> beyond int64_t, ...). Trailing CR/LF dropped.
std::optional<UatLine> parseUatLine(const std::string& line);

// A stratux log's session header, "START,<date>" (trailing CR/LF ok).
bool isStratuxStart(const std::string& line);

// Puts the sessions of one stratux log on one timeline, in file order. A
// session begins at a "START," line (startSession()) or where the offset goes
// backwards; it starts kGapNs after the previous session's last line and
// keeps its own spacing. The first session keeps its offsets as they are.
// next() returns nullopt (state unchanged) if the time would leave int64_t.
class StratuxClock {
public:
    static constexpr int64_t kGapNs = 1'000'000'000;
    void startSession() { restart_ = true; }
    std::optional<int64_t> next(int64_t offset_ns);

private:
    bool have_ = false, restart_ = false;
    int64_t first_ = 0;     // offset of the session's first line
    int64_t start_ = 0;     // its time
    int64_t last_off_ = 0;  // offset and time of the latest line
    int64_t last_ = 0;
};

}  // namespace skycued
