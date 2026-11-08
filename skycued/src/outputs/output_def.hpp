#pragma once

// An output's definition: where the hub sends one kind of raw message, as
// stored in the inputs file ("outputs") and sent over the API. Parsing checks
// every rule; the running state lives in the hub.

#include "inputs/input_def.hpp"
#include "json_lite.hpp"

#include <skycue/net/endpoint.hpp>

#include <cstdint>
#include <string>

namespace skycued {

enum class OutputKind { beast, uat, json, pingstation_json, cat021, cue };

const char* outputKindName(OutputKind kind);  // "beast", "uat", "json", "pingstation_json", "cat021", "cue"

struct OutputDef {
    std::string id, name;
    OutputKind kind = OutputKind::beast;
    bool enabled = true;
    InputTransport transport = InputTransport::tcp_listen;  // required in the JSON
    std::string bind = "0.0.0.0", host, peer, group, iface;  // iface is the "interface" key
    uint16_t port = 0;
    int ttl = 1;
    bool loopback = true;
    bool builtin = false;  // set by the daemon, never parsed

    bool operator==(const OutputDef& o) const;
    bool operator!=(const OutputDef& o) const { return !(*this == o); }
};

// A definition the operator gave that breaks a rule; what() is shown to the
// operator verbatim, as for InputDefError.
struct OutputDefError : InputDefError {
    using InputDefError::InputDefError;
};

// The ids of the cue stream's built-in outputs: cue-tcp, cue-connect-N and
// cue-udp-N (N a number without leading zeros). They cannot be user ids.
bool isReservedCueId(const std::string& id);

// Parses and checks one definition. Throws OutputDefError for an unknown key
// (`builtin` and `state` included: they are output-only), a key that does not
// apply to the transport, a missing kind, transport or port, and a value out
// of its range. NameRule::repair as for parseInputDef.
OutputDef parseOutputDef(const JsonObject& object, NameRule rule = NameRule::strict, bool* repaired = nullptr);

// The definition as one JSON object: the keys that apply to its transport,
// defaults included; never `builtin`. parseOutputDef(parse(outputDefJson(d)))
// == d for a user output.
std::string outputDefJson(const OutputDef& def);

// net::parseEndpoint over the definition (Role::send for udp).
skycue::net::Endpoint toEndpoint(const OutputDef& def);

}  // namespace skycued
