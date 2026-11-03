#include "inputs/inputs_file.hpp"

#include "json_lite.hpp"

#include <set>
#include <utility>

namespace skycued {
namespace {

[[noreturn]] void fail(const std::string& message) { throw InputDefError(message); }

const char* const kBuiltinIds[] = {"readsb", "dump978"};

std::map<std::string, bool> parseBuiltin(const JsonObject& o)
{
    std::map<std::string, bool> out;
    for (const auto& id : o.keys()) {
        bool known = false;
        for (const char* b : kBuiltinIds)
            known |= id == b;
        if (!known)
            fail("builtin: unknown input '" + id + "' (readsb and dump978 only)");
        if (out.count(id))
            fail("builtin: duplicate input '" + id + "'");
        const auto entry = o.getObject(id.c_str());
        if (!entry)
            fail("builtin." + id + " must be an object {\"enabled\":true|false}");
        const auto keys = entry->keys();
        for (const auto& key : keys)
            if (key != "enabled")
                fail("builtin." + id + ": unknown key '" + key + "'");
        if (keys.size() > 1)
            fail("builtin." + id + ": duplicate key 'enabled'");
        const auto enabled = entry->getBool("enabled");
        if (!enabled)
            fail("builtin." + id + ".enabled must be true or false");
        out[id] = *enabled;
    }
    return out;
}

// Which listening socket an enabled input binds: "udp", "tcp", or "" for none.
const char* listenProtocol(const InputDef& d)
{
    if (!d.enabled || d.type == InputType::replay)
        return "";
    if (d.transport == InputTransport::udp)
        return "udp";
    if (d.transport == InputTransport::tcp_listen)
        return "tcp";
    return "";
}

}  // namespace

InputsFile parseInputsFile(const std::string& text)
{
    if (text.size() > kMaxInputsFileBytes)
        fail("the inputs file is larger than 1 MiB");
    const auto root = JsonObject::parse(text, kMaxInputsFileBytes);
    if (!root)
        fail("not a JSON object");
    std::set<std::string> seen;
    for (const auto& key : root->keys()) {
        if (key != "version" && key != "inputs" && key != "builtin")
            fail("unknown key '" + key + "'");
        if (!seen.insert(key).second)
            fail("duplicate key '" + key + "'");
    }
    if (root->getInt("version") != std::optional<int64_t>(1))
        fail("version must be 1");

    InputsFile out;
    if (root->has("inputs")) {
        const auto entries = root->getObjectArray("inputs");
        if (!entries)
            fail("inputs must be an array of objects");
        for (std::size_t i = 0; i < entries->size(); ++i) {
            const JsonObject& entry = (*entries)[i];
            try {
                out.inputs.push_back(parseInputDef(entry));
            } catch (const InputDefError& e) {
                const auto id = entry.getString("id");
                fail("inputs[" + std::to_string(i) + "] (" + (id ? *id : "?") + "): " + e.what());
            }
        }
    }
    if (root->has("builtin")) {
        const auto builtin = root->getObject("builtin");
        if (!builtin)
            fail("builtin must be an object");
        out.builtin_enabled = parseBuiltin(*builtin);
    }
    validateInputSet(out.inputs);
    return out;
}

std::string inputsFileJson(const InputsFile& file)
{
    std::string out = "{\n  \"version\": 1,\n  \"inputs\": [";
    for (std::size_t i = 0; i < file.inputs.size(); ++i)
        out += (i ? ",\n    " : "\n    ") + inputDefJson(file.inputs[i]);
    out += file.inputs.empty() ? "],\n" : "\n  ],\n";
    out += "  \"builtin\": {";
    bool first = true;
    for (const auto& b : file.builtin_enabled) {
        out += first ? "\n    " : ",\n    ";
        appendJsonString(out, b.first);
        out += std::string(": {\"enabled\": ") + (b.second ? "true" : "false") + "}";
        first = false;
    }
    out += file.builtin_enabled.empty() ? "}\n" : "\n  }\n";
    out += "}\n";
    return out;
}

void validateInputSet(const std::vector<InputDef>& defs)
{
    std::set<std::string> ids;
    std::map<std::pair<std::string, uint16_t>, std::string> listeners;  // (protocol, port) -> id
    for (const InputDef& d : defs) {
        if (!ids.insert(d.id).second)
            fail("duplicate id '" + d.id + "'");
        const std::string protocol = listenProtocol(d);
        if (protocol.empty())
            continue;
        const auto inserted = listeners.emplace(std::make_pair(protocol, d.port), d.id);
        if (!inserted.second)
            fail("port " + std::to_string(d.port) + "/" + protocol + " is already used by input " +
                 inserted.first->second);
    }
}

}  // namespace skycued
