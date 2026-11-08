#include "inputs/inputs_file.hpp"

#include "json_lite.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

namespace skycued {
namespace {

[[noreturn]] void fail(const std::string& message) { throw InputDefError(message); }

const char* const kBuiltinIds[] = {"readsb", "dump978"};

std::string needsV2(const std::string& key) { return "'" + key + "' needs inputs file version 2"; }

// The first thing in an input entry that only a version 2 file may hold, or "".
std::string inputV2Key(const JsonObject& e)
{
    const auto type = e.getString("type");
    if (type && (*type == "beast_raw" || *type == "uat_raw"))
        return *type;
    for (const char* key : {"group", "interface", "format", "ref_lat", "ref_lon"})
        if (e.has(key))
            return key;
    if (e.getString("source") == std::optional<std::string>("decode"))
        return "source";
    if (type && e.has("transport") && (*type == "readsb" || *type == "dump978" || *type == "pingstation_json"))
        return "transport";
    return "";
}

// Whether writing `d` needs a version 2 file.
bool inputNeedsV2(const InputDef& d)
{
    if (d.type == InputType::beast_raw || d.type == InputType::uat_raw || !d.group.empty() ||
        d.ref_lat || d.ref_lon || d.source != PingSource::device)
        return true;
    const InputTransport v1 =
        d.type == InputType::pingstation_json ? InputTransport::udp : InputTransport::tcp_connect;
    return d.type != InputType::pingstation_cat021 && d.transport != v1;
}

std::map<std::string, bool> parseBuiltin(const JsonObject& o, int version)
{
    std::map<std::string, bool> out;
    for (const auto& id : o.keys()) {
        bool known = false;
        for (const char* b : kBuiltinIds)
            known |= id == b;
        if (!known && isReservedCueId(id)) {
            if (version < 2)
                fail(needsV2(id));
            known = true;
        }
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
        if (key != "version" && key != "inputs" && key != "outputs" && key != "builtin")
            fail("unknown key '" + key + "'");
        if (!seen.insert(key).second)
            fail("duplicate key '" + key + "'");
    }
    const auto version = root->getInt("version");
    if (version != std::optional<int64_t>(1) && version != std::optional<int64_t>(2))
        fail("version must be 1 or 2");

    InputsFile out;
    out.version = static_cast<int>(*version);
    if (root->has("inputs")) {
        const auto entries = root->getObjectArray("inputs");
        if (!entries)
            fail("inputs must be an array of objects");
        for (std::size_t i = 0; i < entries->size(); ++i) {
            const JsonObject& entry = (*entries)[i];
            try {
                if (out.version < 2) {
                    const std::string key = inputV2Key(entry);
                    if (!key.empty())
                        fail(needsV2(key));
                }
                bool repaired = false;
                out.inputs.push_back(parseInputDef(entry, NameRule::repair, &repaired));
                if (repaired)
                    out.repaired_names.push_back(out.inputs.back().id);
            } catch (const InputDefError& e) {
                const auto id = entry.getString("id");
                fail("inputs[" + std::to_string(i) + "] (" + (id ? *id : "?") + "): " + e.what());
            }
        }
    }
    if (root->has("outputs")) {
        const auto entries = root->getObjectArray("outputs");
        if (!entries)
            fail("outputs must be an array of objects");
        if (out.version < 2 && !entries->empty())
            fail(needsV2("outputs"));
        for (std::size_t i = 0; i < entries->size(); ++i) {
            const JsonObject& entry = (*entries)[i];
            try {
                bool repaired = false;
                out.outputs.push_back(parseOutputDef(entry, NameRule::repair, &repaired));
                if (repaired)
                    out.repaired_output_names.push_back(out.outputs.back().id);
            } catch (const InputDefError& e) {
                const auto id = entry.getString("id");
                fail("outputs[" + std::to_string(i) + "] (" + (id ? *id : "?") + "): " + e.what());
            }
        }
    }
    if (root->has("builtin")) {
        const auto builtin = root->getObject("builtin");
        if (!builtin)
            fail("builtin must be an object");
        out.builtin_enabled = parseBuiltin(*builtin, out.version);
    }
    out.warnings = validateInputSet(out.inputs, out.outputs);
    return out;
}

std::string inputsFileJson(const InputsFile& file)
{
    bool v2 = !file.outputs.empty() || std::any_of(file.inputs.begin(), file.inputs.end(), inputNeedsV2);
    // Enabled is the default: a cue id that is true carries nothing, so it is not written.
    const auto cueContent = [](const std::pair<const std::string, bool>& b) { return isReservedCueId(b.first) && !b.second; };
    for (const auto& b : file.builtin_enabled)
        v2 = v2 || cueContent(b);
    std::string out = std::string("{\n  \"version\": ") + (v2 ? "2" : "1") + ",\n  \"inputs\": [";
    for (std::size_t i = 0; i < file.inputs.size(); ++i)
        out += (i ? ",\n    " : "\n    ") + inputDefJson(file.inputs[i]);
    out += file.inputs.empty() ? "],\n" : "\n  ],\n";
    if (v2) {
        out += "  \"outputs\": [";
        for (std::size_t i = 0; i < file.outputs.size(); ++i)
            out += (i ? ",\n    " : "\n    ") + outputDefJson(file.outputs[i]);
        out += file.outputs.empty() ? "],\n" : "\n  ],\n";
    }
    out += "  \"builtin\": {";
    bool first = true;
    for (const auto& b : file.builtin_enabled) {
        if (isReservedCueId(b.first) && b.second)
            continue;
        out += first ? "\n    " : ",\n    ";
        appendJsonString(out, b.first);
        out += std::string(": {\"enabled\": ") + (b.second ? "true" : "false") + "}";
        first = false;
    }
    out += first ? "}\n" : "\n  }\n";
    out += "}\n";
    return out;
}

namespace {

struct Listener {
    std::string owner;  // "input ps" or "output beast-out"
    bool multicast;     // a receiver with a group
};

bool isLoopbackHost(const std::string& host)
{
    if (host.compare(0, 4, "127.") == 0 && isIpv4Literal(host))
        return true;
    std::string lower = host;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (!lower.empty() && lower.back() == '.')
        lower.pop_back();
    return lower == "localhost";
}

}  // namespace

std::vector<std::string> validateInputSet(const std::vector<InputDef>& inputs, const std::vector<OutputDef>& outputs,
                                          const std::vector<ReservedPort>& reserved)
{
    std::set<std::string> ids;
    for (const InputDef& d : inputs)
        if (!ids.insert(d.id).second)
            fail("duplicate id '" + d.id + "'");
    std::set<std::string> output_ids;
    for (const OutputDef& d : outputs)
        if (!output_ids.insert(d.id).second)
            fail("duplicate output id '" + d.id + "'");

    std::map<std::pair<std::string, uint16_t>, Listener> listeners;  // (protocol, port) -> the first
    auto claim = [&](const std::string& protocol, uint16_t port, const std::string& owner, bool multicast) {
        const auto inserted = listeners.emplace(std::make_pair(protocol, port), Listener{owner, multicast});
        if (inserted.second)
            return;
        if (multicast && inserted.first->second.multicast)
            return;  // multicast receivers share a port whatever their groups
        fail("port " + std::to_string(port) + "/" + protocol + " is already used by " + inserted.first->second.owner);
    };
    for (const InputDef& d : inputs) {
        const std::string protocol = listenProtocol(d);
        if (!protocol.empty())
            claim(protocol, d.port, "input " + d.id, !d.group.empty());
    }
    for (const OutputDef& d : outputs) {
        if (!d.enabled || d.transport != InputTransport::tcp_listen)
            continue;
        claim("tcp", d.port, "output " + d.id, false);
        for (const ReservedPort& r : reserved)
            if (r.protocol == "tcp" && r.port == d.port)
                fail("port " + std::to_string(d.port) + "/tcp is already used by " + r.owner);
    }

    std::vector<std::string> warnings;
    for (const OutputDef& o : outputs) {
        if (!o.enabled || o.transport != InputTransport::udp)
            continue;
        if (!o.group.empty()) {
            for (const InputDef& d : inputs)
                if (d.enabled && d.transport == InputTransport::udp && d.group == o.group && d.port == o.port)
                    fail("output '" + o.id + "' sends to " + o.group + ":" + std::to_string(o.port) + ", which input '" +
                         d.id + "' joins (a loop)");
        }
    }
    for (const OutputDef& o : outputs) {
        if (!o.enabled || o.transport == InputTransport::tcp_listen || !o.group.empty() || !isLoopbackHost(o.host))
            continue;
        const std::string protocol = o.transport == InputTransport::udp ? "udp" : "tcp";
        const auto found = listeners.find(std::make_pair(protocol, o.port));
        if (found == listeners.end())
            continue;
        const std::string& owner = found->second.owner;  // "input x" or "output x"
        const bool is_input = owner.compare(0, 6, "input ") == 0;
        const std::string name = owner.substr(is_input ? 6 : 7);
        warnings.push_back("output '" + o.id + "' sends to " + o.host + ":" + std::to_string(o.port) + ", which " +
                           (is_input ? "input '" : "output '") + name + "' of this daemon " +
                           (is_input ? "listens on" : "serves"));
    }
    return warnings;
}

std::vector<std::string> decoderWarnings(const std::vector<InputDef>& inputs, const std::vector<OutputDef>& outputs)
{
    std::vector<std::string> warnings;
    const InputDef* beast = nullptr;
    for (const InputDef& d : inputs)
        if (d.enabled && d.type == InputType::beast_raw && (!beast || (d.builtin && !beast->builtin)))
            beast = &d;
    const InputDef* uat = nullptr;
    for (const InputDef& d : inputs)
        if (d.enabled && (d.type == InputType::uat_raw || d.type == InputType::dump978) &&
            (!uat || (d.builtin && !uat->builtin)))
            uat = &d;
    bool json_input = false;
    for (const InputDef& d : inputs) {
        if (!d.enabled || d.type != InputType::readsb)
            continue;
        json_input = true;
        if (uat)
            warnings.push_back("TRACK_DECODER=skycued: input '" + d.id + "' (readsb JSON) and input '" + uat->id +
                               "' (" + inputTypeName(uat->type) +
                               ") both feed the track table, so 978 MHz aircraft are counted twice; disable one");
        if (beast)
            warnings.push_back("TRACK_DECODER=skycued: input '" + d.id + "' (readsb JSON) and input '" + beast->id +
                               "' (beast_raw) both feed the track table, so aircraft are counted twice; disable one");
    }
    if (!json_input)
        for (const OutputDef& o : outputs)
            if (o.enabled && o.kind == OutputKind::json) {
                warnings.push_back(
                    "TRACK_DECODER=skycued: the json output '" + o.id +
                    "' carries nothing, since no readsb JSON input is enabled; it stays defined and enabled");
                break;
            }
    return warnings;
}

}  // namespace skycued
