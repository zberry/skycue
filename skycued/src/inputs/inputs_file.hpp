#pragma once

// skycue.inputs.json (spec §4.3): the user inputs and the enabled state of
// the built-ins, the single source of truth for both.
//
//   {"version":1,
//    "inputs":[{definition}, ...],
//    "builtin":{"readsb":{"enabled":true},"dump978":{"enabled":true}}}
//
// "inputs" and "builtin" may be left out. "inputs":{} reads as an empty list
// (PropertyTree cannot tell {} from []).

#include "inputs/input_def.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace skycued {

constexpr std::size_t kMaxInputsFileBytes = 1024 * 1024;

struct InputsFile {
    std::vector<InputDef> inputs;              // user inputs, in file order
    std::map<std::string, bool> builtin_enabled;  // "readsb"/"dump978" -> enabled; absent = default
};

// Parses and checks the file's text. Throws InputDefError for: text over
// kMaxInputsFileBytes, not a JSON object, a version other than 1 (or none),
// an unknown top-level key, "inputs" not an array of objects, a bad entry
// (the message is prefixed "inputs[N] (id): ", with "?" for a missing id), a
// "builtin" entry for an id other than readsb and dump978 or with anything but
// a boolean "enabled", and any problem validateInputSet finds.
InputsFile parseInputsFile(const std::string& text);

// The file as indented text ending in a newline; one definition per line.
std::string inputsFileJson(const InputsFile& file);

// Checks a whole set: throws InputDefError for a duplicate id ("duplicate id
// 'ps'") or for two enabled listeners on the same port and protocol (udp:
// pingstation_json and pingstation_cat021/udp; tcp: pingstation_cat021/
// tcp_listen), naming the first: "port 4000/udp is already used by input
// ps-north". Outgoing connections never conflict.
void validateInputSet(const std::vector<InputDef>& defs);

}  // namespace skycued
