#pragma once

// One raw message as an input received it, on its way to the hub's outputs.

#include <skycue/net/buffer.hpp>

#include <chrono>
#include <string_view>

namespace skycued {

enum class RawKind { beast, uat, json, pingstation_json, cat021 };

inline const char* rawKindName(RawKind kind)
{
    switch (kind) {
    case RawKind::beast: return "beast";
    case RawKind::uat: return "uat";
    case RawKind::json: return "json";
    case RawKind::pingstation_json: return "pingstation_json";
    case RawKind::cat021: return "cat021";
    }
    return "?";
}

// Valid during the call; a sink that keeps the message keeps the Buf.
// bytes: beast, the frame as on the wire (escaped); uat and json, the line
// without its terminator; pingstation_json, the JSON text as received;
// cat021, one data block.
struct RawMessage {
    RawKind kind = RawKind::beast;
    skycue::net::Buf bytes;
    std::chrono::system_clock::time_point arrived;
    std::string_view input;  // id of the input it came from
};

}  // namespace skycued
