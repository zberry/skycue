#pragma once

// skycue wire protocol, version 2. See docs/protocol.md for the byte layout.
//
// Most consumers never use this header directly: skycue::Client decodes the
// stream and hands out Track values. It is public for tools that inspect raw
// frames (Client::onFrame, skycue-dump), for tests, and for writing a sender.

#include <skycue/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace skycue {

constexpr char kMagic[4] = {'S', 'K', 'Y', 'C'}; // first four bytes of every frame
constexpr uint8_t kVersion = 2;                  // the version this library sends; it also reads version 1
// The header's type byte. A frame may carry a value not listed here (see Frame).
enum class FrameType : uint8_t { hello = 1, track_update = 2, track_drop = 3, heartbeat = 4 };
constexpr size_t kHeaderSize = 8;       // magic, version, type, u16 payload length
constexpr size_t kMaxUdpPayload = 1200; // largest payload sent or accepted in one UDP datagram

// Wire limits for TrackUpdate strings; longer values are truncated on encode
// and rejected on decode.
constexpr size_t kMaxCallsignLen = 8;
constexpr size_t kMaxSquawkLen = 4;
constexpr size_t kMaxRegistrationLen = 16;
// Decoded TrackUpdate position_time_ms values above this (2100-01-01T00:00:00Z)
// are clamped to it.
constexpr uint64_t kMaxPositionTimeMs = 4102444800000ULL;
// Hello sender is truncated to this on encode.
constexpr size_t kMaxSenderLen = 255;

// Sent first on every TCP connection and, over UDP, before every keyframe
// burst. Intervals of 0 mean "not stated" and leave the client's setting alone.
struct Hello {
    uint32_t keyframe_interval_ms{};  // how often the sender repeats every track as a keyframe
    uint32_t heartbeat_interval_ms{}; // longest the sender stays silent before sending a Heartbeat
    std::string sender;               // free-form sender name, e.g. "skycued"
};

// The sender has stopped forwarding a track.
struct TrackDrop {
    TrackId id;
    DropReason reason{DropReason::timeout};
};

// Sent when nothing else has been sent for heartbeat_interval_ms; liveness only.
struct Heartbeat {
    uint64_t sender_time_ms{}; // sender's wall clock, ms since the Unix epoch (informational)
};

// One decoded frame. `type` carries the raw byte from the header, so it may
// hold a value not listed in FrameType; callers skip such frames.
struct Frame {
    FrameType type{};             // raw header type byte
    std::vector<uint8_t> payload; // the bytes after the header, undecoded
    uint8_t version{kVersion};    // the header's version byte (1 or 2); selects the TrackUpdate layout
};

// Result of decodeFrame. ok: one frame decoded. need_more: the buffer ends
// inside a header or payload; wait for more bytes. bad_magic, bad_version:
// not a skycue frame of a supported version; a stream cannot be resynchronised after this.
enum class DecodeStatus { ok, need_more, bad_magic, bad_version };

// Each encoder returns a complete frame: header followed by payload.
std::vector<uint8_t> encodeHello(const Hello& h);           // sender truncated to kMaxSenderLen
std::vector<uint8_t> encodeTrackUpdate(const Track& t);     // strings truncated; lat/lon clamped, non-finite -> 0
std::vector<uint8_t> encodeTrackDrop(const TrackDrop& d);
std::vector<uint8_t> encodeHeartbeat(const Heartbeat& hb);

// Decodes one frame from the start of [data, data+len). On ok, `out` holds the
// frame and `consumed` is header + payload length. On any other status
// `consumed` is 0 and `out` is unchanged. need_more means the buffer holds a
// partial header or a partial payload. The magic and version are checked as
// soon as the full header is available. Versions 1 through max_version are
// accepted (default kVersion); a reader that only speaks version 1 passes 1.
DecodeStatus decodeFrame(const uint8_t* data, size_t len, Frame& out, size_t& consumed,
                         uint8_t max_version = kVersion);

// Payload decoders. Each returns false if the frame type does not match or the
// payload is truncated or malformed; `out` is then unspecified. Trailing bytes
// beyond the fields this version knows are ignored.
bool decodeHello(const Frame& f, Hello& out);
// Enum bytes are passed through unchanged (may be outside the known set). Reads the
// layout of f.version (1: u16 presence, bits 0-10; 2: u32 presence, bits 0-25). At a
// presence bit this version does not know, stops and keeps the fields read so far.
bool decodeTrackUpdate(const Frame& f, Track& out);
bool decodeTrackDrop(const Frame& f, TrackDrop& out); // reason byte passed through unchanged
bool decodeHeartbeat(const Frame& f, Heartbeat& out);

} // namespace skycue
