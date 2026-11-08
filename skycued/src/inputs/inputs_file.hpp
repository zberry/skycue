#pragma once

// skycue.inputs.json (spec §4.3): the user inputs and the enabled state of
// the built-ins, the single source of truth for both.
//
//   {"version":1,
//    "inputs":[{definition}, ...],
//    "builtin":{"readsb":{"enabled":true},"dump978":{"enabled":true}}}
//
// Version 2 adds "outputs":[{definition}, ...] (after "inputs"), the input
// types beast_raw and uat_raw, a transport on every input type, the keys
// group, interface and format, and the cue stream's ids (cue-tcp,
// cue-connect-N, cue-udp-N) in "builtin". A file is written as version 2 only
// when it holds some of that; otherwise as version 1, in the v1 shape. A
// version 1 header over version 2 content is an error naming the key.
//
// "inputs", "outputs" and "builtin" may be left out. "inputs":{} reads as an empty list
// (PropertyTree cannot tell {} from []).

#include "inputs/input_def.hpp"
#include "outputs/output_def.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace skycued {

constexpr std::size_t kMaxInputsFileBytes = 1024 * 1024;

struct InputsFile {
    int version = 1;                           // as read; inputsFileJson applies the version rule
    std::vector<InputDef> inputs;              // user inputs, in file order
    std::vector<OutputDef> outputs;            // user outputs, in file order (version 2)
    // "readsb"/"dump978" and, in version 2, the cue stream's ids -> enabled; absent = default
    std::map<std::string, bool> builtin_enabled;
    std::vector<std::string> repaired_names;      // ids whose stored name broke the name rule and was repaired
    std::vector<std::string> repaired_output_names;  // the same for outputs
    std::vector<std::string> warnings;            // validateInputSet's WARN texts
};

// Parses and checks the file's text. Throws InputDefError for: text over
// kMaxInputsFileBytes, not a JSON object, a version other than 1 or 2 (or
// none: "version must be 1 or 2"), an unknown top-level key, "inputs" or
// "outputs" not an array of objects, a bad entry (the message is prefixed
// "inputs[N] (id): " or "outputs[N] (id): ", with "?" for a missing id), a
// "builtin" entry for an id other than readsb, dump978 and (version 2) the
// cue ids, or with anything but a boolean "enabled", version 2 content under
// a version 1 header ("'outputs' needs inputs file version 2"), and any
// problem validateInputSet finds. A stored
// "name" that breaks the name rule is not a bad entry: it is repaired
// (NameRule::repair) and its id listed in repaired_names.
InputsFile parseInputsFile(const std::string& text);

// The file as indented text ending in a newline; one definition per line.
std::string inputsFileJson(const InputsFile& file);

// Checks a whole set. Throws InputDefError for a duplicate input id
// ("duplicate id 'ps'") or output id ("duplicate output id 'x'"; the two
// namespaces are separate); for two enabled listeners (inputs that receive on
// udp or tcp_listen, outputs with tcp_listen) on the same protocol and port,
// naming the first, inputs before outputs: "port 4000/udp is already used by
// input ps-north" or "... by output beast-out" (receivers with a multicast
// group share a port whatever their groups; a unicast and a multicast
// receiver conflict); for an enabled output on a `reserved` port ("... by the
// cue stream (CUE_TCP_PORT)"; inputs are not checked against reserved ports);
// and for an enabled udp output that sends to a group and port an enabled
// input joins (a loop). A udp output binds nothing and never conflicts.
// Outgoing connections never conflict. Returns WARN texts: an enabled output
// that sends to a loopback unicast address (127.0.0.0/8 or localhost) and a
// port that an enabled input of this set listens on or an enabled output
// serves.
struct ReservedPort {
    std::string protocol;  // "tcp" or "udp"
    uint16_t port = 0;
    std::string owner;     // e.g. "the cue stream (CUE_TCP_PORT)"
};
std::vector<std::string> validateInputSet(const std::vector<InputDef>& inputs,
                                          const std::vector<OutputDef>& outputs = {},
                                          const std::vector<ReservedPort>& reserved = {});

}  // namespace skycued
