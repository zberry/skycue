#pragma once

// skycue wire protocol, version 1. See docs/protocol.md for the byte layout.

#include <skycue/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace skycue {

constexpr char kMagic[4] = {'S', 'K', 'Y', 'C'};
constexpr uint8_t kVersion = 1;
enum class FrameType : uint8_t { hello = 1, track_update = 2, track_drop = 3, heartbeat = 4 };
constexpr size_t kHeaderSize = 8;
constexpr size_t kMaxUdpPayload = 1200;

// Wire limits for TrackUpdate strings; longer values are truncated on encode
// and rejected on decode.
constexpr size_t kMaxCallsignLen = 8;
constexpr size_t kMaxSquawkLen = 4;
constexpr size_t kMaxRegistrationLen = 16;
// Hello sender is truncated to this on encode.
constexpr size_t kMaxSenderLen = 255;

struct Hello {
    uint32_t keyframe_interval_ms{};
    uint32_t heartbeat_interval_ms{};
    std::string sender;
};

struct TrackDrop {
    TrackId id;
    DropReason reason{DropReason::timeout};
};

struct Heartbeat {
    uint64_t sender_time_ms{};
};

// One decoded frame. `type` carries the raw byte from the header, so it may
// hold a value not listed in FrameType; callers skip such frames.
struct Frame {
    FrameType type{};
    std::vector<uint8_t> payload;
};

enum class DecodeStatus { ok, need_more, bad_magic, bad_version };

// Each encoder returns a complete frame: header followed by payload.
std::vector<uint8_t> encodeHello(const Hello& h);
std::vector<uint8_t> encodeTrackUpdate(const Track& t);
std::vector<uint8_t> encodeTrackDrop(const TrackDrop& d);
std::vector<uint8_t> encodeHeartbeat(const Heartbeat& hb);

// Decodes one frame from the start of [data, data+len). On ok, `out` holds the
// frame and `consumed` is header + payload length. On any other status
// `consumed` is 0 and `out` is unchanged. need_more means the buffer holds a
// partial header or a partial payload. The magic and version are checked as
// soon as the full header is available.
DecodeStatus decodeFrame(const uint8_t* data, size_t len, Frame& out, size_t& consumed);

// Payload decoders. Each returns false if the frame type does not match or the
// payload is truncated or malformed; `out` is then unspecified. Trailing bytes
// beyond the fields this version knows are ignored.
bool decodeHello(const Frame& f, Hello& out);
bool decodeTrackUpdate(const Frame& f, Track& out);
bool decodeTrackDrop(const Frame& f, TrackDrop& out);
bool decodeHeartbeat(const Frame& f, Heartbeat& out);

} // namespace skycue
