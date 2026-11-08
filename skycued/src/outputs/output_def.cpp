#include "outputs/output_def.hpp"

#include <algorithm>
#include <iterator>
#include <set>

namespace skycued {
namespace {

const char* const kKeys[] = {"id",   "name",  "kind", "enabled", "transport", "bind", "host",
                             "port", "peer",  "group", "interface", "ttl",      "loopback"};

[[noreturn]] void fail(const std::string& message) { throw OutputDefError(message); }

bool isNull(const JsonObject& o, const char* key)
{
    const auto text = o.getString(key);
    return text && *text == "null";
}

std::string getText(const JsonObject& o, const char* key, const char* what)
{
    const auto text = o.getString(key);
    if (!text)
        fail(std::string(key) + " must be " + what);
    return *text;
}

int64_t getIntIn(const JsonObject& o, const char* key, int64_t lo, int64_t hi)
{
    const auto value = o.getInt(key);
    if (!value || *value < lo || *value > hi)
        fail(std::string(key) + " must be an integer from " + std::to_string(lo) + " to " + std::to_string(hi));
    return *value;
}

bool getFlag(const JsonObject& o, const char* key)
{
    const auto value = o.getBool(key);
    if (!value)
        fail(std::string(key) + " must be true or false");
    return *value;
}

std::string getOptionalText(const JsonObject& o, const char* key, const char* what)
{
    if (!o.has(key) || isNull(o, key))
        return "";
    return getText(o, key, what);
}

bool allDigitsNoLeadingZero(const std::string& s)
{
    if (s.empty() || s.size() > 6 || (s.size() > 1 && s[0] == '0'))
        return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::optional<OutputKind> parseKind(const std::string& s)
{
    for (OutputKind k : {OutputKind::beast, OutputKind::uat, OutputKind::json, OutputKind::pingstation_json,
                         OutputKind::cat021, OutputKind::cue})
        if (s == outputKindName(k))
            return k;
    return std::nullopt;
}

std::optional<InputTransport> parseTransport(const std::string& s)
{
    for (InputTransport t : {InputTransport::tcp_listen, InputTransport::tcp_connect, InputTransport::udp})
        if (s == transportName(t))
            return t;
    return std::nullopt;
}

void appendOptionalText(JsonWriter& w, const char* key, const std::string& v)
{
    if (v.empty())
        w.addRaw(key, "null");
    else
        w.add(key, v);
}

}  // namespace

const char* outputKindName(OutputKind kind)
{
    switch (kind) {
    case OutputKind::beast: return "beast";
    case OutputKind::uat: return "uat";
    case OutputKind::json: return "json";
    case OutputKind::pingstation_json: return "pingstation_json";
    case OutputKind::cat021: return "cat021";
    case OutputKind::cue: return "cue";
    }
    return "?";
}

bool OutputDef::operator==(const OutputDef& o) const
{
    return id == o.id && name == o.name && kind == o.kind && enabled == o.enabled && transport == o.transport &&
           bind == o.bind && host == o.host && peer == o.peer && group == o.group && iface == o.iface &&
           port == o.port && ttl == o.ttl && loopback == o.loopback && builtin == o.builtin;
}

bool isReservedCueId(const std::string& id)
{
    if (id == "cue-tcp")
        return true;
    const std::string connect = "cue-connect-", udp = "cue-udp-";
    if (id.compare(0, connect.size(), connect) == 0)
        return allDigitsNoLeadingZero(id.substr(connect.size()));
    if (id.compare(0, udp.size(), udp) == 0)
        return allDigitsNoLeadingZero(id.substr(udp.size()));
    return false;
}

OutputDef parseOutputDef(const JsonObject& o, NameRule rule, bool* repaired)
{
    std::set<std::string> seen;
    for (const auto& key : o.keys()) {
        if (std::find(std::begin(kKeys), std::end(kKeys), key) == std::end(kKeys))
            fail("unknown key '" + clipForMessage(key) + "'");
        if (!seen.insert(key).second)
            fail("duplicate key '" + clipForMessage(key) + "'");
    }

    OutputDef d;
    if (!o.has("id"))
        fail("id is required");
    d.id = getText(o, "id", "a string");
    if (!isValidInputId(d.id))
        fail("id must be 1 to 32 characters of a-z, 0-9 and -");
    if (isReservedCueId(d.id))
        fail("id '" + d.id + "' is reserved");

    const auto kind = o.has("kind") ? parseKind(getText(o, "kind", "a string")) : std::nullopt;
    if (!kind)
        fail("kind must be one of beast, uat, json, pingstation_json, cat021, cue");
    d.kind = *kind;
    if (!o.has("transport"))
        fail("transport is required for outputs (tcp_listen, tcp_connect or udp)");
    const auto transport = parseTransport(getText(o, "transport", "tcp_listen, tcp_connect or udp"));
    if (!transport)
        fail("transport must be tcp_listen, tcp_connect or udp");
    d.transport = *transport;

    const bool listen = d.transport == InputTransport::tcp_listen;
    const bool udp = d.transport == InputTransport::udp;
    const bool connect = d.transport == InputTransport::tcp_connect;
    for (const auto& key : o.keys()) {
        if ((key == "bind" || key == "peer") && !listen)
            fail(key + " applies to tcp_listen only");
        if (key == "host" && listen)
            fail("host is not used with tcp_listen");
        if (key == "group" && !udp)
            fail("group applies to udp only");
    }

    if (o.has("name")) {
        d.name = getText(o, "name", "a string");
        if (!isValidInputName(d.name)) {
            if (rule != NameRule::repair)
                fail("name must be at most 64 printable characters");
            d.name = repairInputName(d.name);
            if (repaired != nullptr)
                *repaired = true;
        }
    }
    if (o.has("enabled"))
        d.enabled = getFlag(o, "enabled");
    if (!o.has("port"))
        fail("port is required");
    d.port = static_cast<uint16_t>(getIntIn(o, "port", 1, 65535));

    if (listen) {
        if (o.has("bind")) {
            d.bind = getText(o, "bind", "an IPv4 address");
            if (!isIpv4Literal(d.bind))
                fail("bind must be an IPv4 address");
        }
        d.peer = getOptionalText(o, "peer", "an IPv4 address");
        if (!d.peer.empty() && !isIpv4Literal(d.peer))
            fail("peer must be an IPv4 address");
    }
    if (connect || udp) {
        d.host = getOptionalText(o, "host", "a string");
        if (connect && d.host.empty())
            fail("host is required");
        if (!d.host.empty() && !isValidHost(d.host))
            fail("host must be an IPv4 address or a hostname of at most 253 characters");
    }
    if (udp) {
        d.group = getOptionalText(o, "group", "a multicast address");
        if (!d.group.empty() && !isIpv4Multicast(d.group))
            fail("group must be a multicast address (224.0.0.0 to 239.255.255.255)");
        if (d.host.empty() && d.group.empty())
            fail("udp needs a host (unicast) or a group (multicast)");
        if (!d.host.empty() && !d.group.empty())
            fail("host and group cannot both be set");
    }
    for (const char* key : {"interface", "ttl", "loopback"}) {
        if (o.has(key) && d.group.empty())
            fail(std::string(key) + " applies to a multicast group only");
    }
    if (!d.group.empty()) {
        d.iface = getOptionalText(o, "interface", "an IPv4 address");
        if (!d.iface.empty() && !isIpv4Literal(d.iface))
            fail("interface must be an IPv4 address");
        if (o.has("ttl"))
            d.ttl = static_cast<int>(getIntIn(o, "ttl", 1, 255));
        if (o.has("loopback"))
            d.loopback = getFlag(o, "loopback");
    }
    return d;
}

std::string outputDefJson(const OutputDef& d)
{
    JsonWriter w;
    w.add("id", d.id).add("name", d.name).add("kind", outputKindName(d.kind)).add("enabled", d.enabled);
    w.add("transport", transportName(d.transport));
    switch (d.transport) {
    case InputTransport::tcp_listen:
        w.add("bind", d.bind).add("port", static_cast<int64_t>(d.port));
        appendOptionalText(w, "peer", d.peer);
        break;
    case InputTransport::tcp_connect:
        w.add("host", d.host).add("port", static_cast<int64_t>(d.port));
        break;
    case InputTransport::udp:
        if (d.group.empty()) {
            w.add("host", d.host).add("port", static_cast<int64_t>(d.port));
        } else {
            w.add("port", static_cast<int64_t>(d.port)).add("group", d.group);
            appendOptionalText(w, "interface", d.iface);
            w.add("ttl", static_cast<int64_t>(d.ttl)).add("loopback", d.loopback);
        }
        break;
    }
    return w.str();
}

skycue::net::Endpoint toEndpoint(const OutputDef& d)
{
    skycue::net::EndpointFields f;
    f.transport = transportName(d.transport);
    f.port = std::to_string(d.port);
    switch (d.transport) {
    case InputTransport::tcp_listen:
        f.bind = d.bind;
        f.peer = d.peer;
        break;
    case InputTransport::tcp_connect:
        f.host = d.host;
        break;
    case InputTransport::udp:
        f.host = d.host;
        f.group = d.group;
        if (!d.group.empty()) {
            f.iface = d.iface;
            f.ttl = std::to_string(d.ttl);
            f.loopback = d.loopback ? "true" : "false";
        }
        break;
    }
    return skycue::net::parseEndpoint(f, d.transport == InputTransport::udp ? skycue::net::Role::send
                                                                           : skycue::net::Role::receive);
}

}  // namespace skycued
