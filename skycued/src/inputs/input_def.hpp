#pragma once

// An input's definition (spec §4.1): what the operator configures, as stored
// in the inputs file and sent over the API. Parsing checks every rule; the
// running state (connected, counters) lives elsewhere.

#include "inputs/input_types.hpp"
#include "json_lite.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace skycued {

// `replay` is internal: main builds it for --replay. It is never accepted
// from the API or the inputs file, so users have the other four.
enum class InputType { readsb, dump978, pingstation_json, pingstation_cat021, replay };
enum class InputTransport { tcp_connect, udp, tcp_listen };

const char* inputTypeName(InputType type);            // "pingstation_cat021", ...
const char* transportName(InputTransport transport);  // "tcp_listen", ...
// Whether the input is a track input (counts for source_lost, spec §4.4):
// every type but dump978.
bool isTrackType(InputType type);

struct InputDef {
    std::string id, name;
    InputType type = InputType::readsb;
    bool enabled = true;
    // readsb and dump978 are always tcp_connect, pingstation_json always udp;
    // pingstation_cat021 chooses.
    InputTransport transport = InputTransport::tcp_connect;
    std::string bind = "0.0.0.0", host;  // bind: udp/tcp_listen; host: tcp_connect
    uint16_t port = 0;
    std::string edition = "2.5";       // pingstation_cat021
    std::optional<uint8_t> sac, sic;   // pingstation_cat021
    std::string status_url;            // pingstation_*; "" = none
    int status_interval_s = 30;        // pingstation_*
    TimeMode time_mode = TimeMode::utc;  // pingstation_*
    bool capture = false;              // pingstation_*
    std::string peer;                  // udp/tcp_listen; "" = any
    bool builtin = false;              // set by the daemon, never parsed

    bool operator==(const InputDef& o) const;
    bool operator!=(const InputDef& o) const { return !(*this == o); }
};

// A definition the operator gave that breaks a rule. what() is shown to the
// operator verbatim ("port must be an integer from 1 to 65535").
struct InputDefError : std::runtime_error {
    using runtime_error::runtime_error;
};

// Parses and checks one definition (every rule of spec §4.1). Throws
// InputDefError for: an unknown key (`builtin` and `state` included: they are
// output-only), a key that does not apply to the type or transport, a missing
// required field (id, type and port always; host for readsb, dump978 and
// tcp_connect), a value out of its range, a reserved id (readsb, dump978,
// replay) or the type "replay". Optional values may be null: sac, sic,
// status_url and peer (status_url and peer may also be "").
//
// PropertyTree keeps every scalar as its text, so a quoted number reads as
// the number ("80" as 80) and "true" as true.
InputDef parseInputDef(const JsonObject& object);

// The id rule: 1 to 32 characters of a-z, 0-9 and '-' (reserved ids pass).
bool isValidInputId(const std::string& id);
// The rule for a definition's "name": valid UTF-8, at most 64 characters, no
// C0 or C1 control character and no DEL.
bool isValidInputName(const std::string& name);
// `text` for an error message: as is up to 64 bytes, else its first 64
// bytes (never splitting a UTF-8 character) and "...".
std::string clipForMessage(const std::string& text);

// The definition as one JSON object: only the keys that apply to its type and
// transport, defaults included (unset optionals as null); never `builtin`.
// For a user input, parseInputDef(parse(inputDefJson(d))) == d.
std::string inputDefJson(const InputDef& def);

// An http:// URL split for a request.
struct HttpUrl {
    std::string host;
    uint16_t port = 80;
    std::string target = "/";  // path and query
};

// Accepts "http://host[:port][/target]" only: no other scheme, no userinfo, a
// port of 1..65535, a host as for InputDef::host (IPv4 or hostname), no
// spaces or control characters. nullopt otherwise.
std::optional<HttpUrl> parseHttpUrl(const std::string& url);

}  // namespace skycued
