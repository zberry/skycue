#include "inputs/input_def.hpp"

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <set>

namespace skycued {
namespace {

constexpr std::size_t kMaxIdChars = 32;
constexpr std::size_t kMaxNameChars = 64;
constexpr std::size_t kMaxHostChars = 253;
constexpr std::size_t kMaxLabelChars = 63;
constexpr int64_t kMinStatusInterval = 5, kMaxStatusInterval = 3600;

const char* const kKeys[] = {"id",      "name", "type", "enabled",           "transport", "bind",
                             "host",    "port", "peer", "edition",           "sac",       "sic",
                             "status_url",      "status_interval_s",         "time_mode", "capture",
                             "group",   "interface",    "format",
                             "ref_lat", "ref_lon", "source"};
const char* const kReservedIds[] = {"readsb", "dump978", "replay"};
const char* const kEditions[] = {"2.2", "2.3", "2.4", "2.5", "2.6", "2.7"};

[[noreturn]] void fail(const std::string& message) { throw InputDefError(message); }

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlnum(char c) { return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

}  // namespace

bool isValidInputId(const std::string& id)
{
    if (id.empty() || id.size() > kMaxIdChars)
        return false;
    return std::all_of(id.begin(), id.end(),
                       [](char c) { return isDigit(c) || (c >= 'a' && c <= 'z') || c == '-'; });
}

// Valid UTF-8 (shortest form, no surrogates, at most U+10FFFF) of at most
// kMaxNameChars code points, none a C0 or C1 control character or DEL.
bool isValidInputName(const std::string& name)
{
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < name.size()) {
        const auto u = static_cast<unsigned char>(name[i]);
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if (u < 0x80) {
            len = 1;
            cp = u;
        } else if (u >= 0xC2 && u <= 0xDF) {
            len = 2;
            cp = u & 0x1Fu;
        } else if (u >= 0xE0 && u <= 0xEF) {
            len = 3;
            cp = u & 0x0Fu;
        } else if (u >= 0xF0 && u <= 0xF4) {
            len = 4;
            cp = u & 0x07u;
        } else {
            return false;  // a continuation byte, an overlong lead (C0, C1) or F5..FF
        }
        if (name.size() - i < len)
            return false;  // truncated
        for (std::size_t k = 1; k < len; ++k) {
            const auto c = static_cast<unsigned char>(name[i + k]);
            if ((c & 0xC0u) != 0x80u)
                return false;
            cp = (cp << 6) | (c & 0x3Fu);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;  // overlong, out of range or a surrogate
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F))
            return false;  // C0, DEL, C1
        i += len;
        ++chars;
    }
    return chars <= kMaxNameChars;
}

namespace {

// The code point of the well-formed UTF-8 sequence at name[i] and its
// length, or a length of 0 when there is none (the isValidInputName rules).
std::size_t utf8At(const std::string& name, std::size_t i, std::uint32_t& cp)
{
    const auto u = static_cast<unsigned char>(name[i]);
    std::size_t len = 0;
    if (u < 0x80) {
        cp = u;
        return 1;
    } else if (u >= 0xC2 && u <= 0xDF) {
        len = 2;
        cp = u & 0x1Fu;
    } else if (u >= 0xE0 && u <= 0xEF) {
        len = 3;
        cp = u & 0x0Fu;
    } else if (u >= 0xF0 && u <= 0xF4) {
        len = 4;
        cp = u & 0x07u;
    } else {
        return 0;
    }
    if (name.size() - i < len)
        return 0;
    for (std::size_t k = 1; k < len; ++k) {
        const auto c = static_cast<unsigned char>(name[i + k]);
        if ((c & 0xC0u) != 0x80u)
            return 0;
        cp = (cp << 6) | (c & 0x3Fu);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return len;
}

}  // namespace

bool isIpv4Literal(const std::string& s)
{
    int parts = 0;
    std::size_t i = 0;
    while (true) {
        const std::size_t start = i;
        int value = 0;
        while (i < s.size() && isDigit(s[i]) && i - start < 3)
            value = value * 10 + (s[i++] - '0');
        const std::size_t len = i - start;
        if (len == 0 || value > 255 || (len > 1 && s[start] == '0'))
            return false;
        ++parts;
        if (i == s.size())
            return parts == 4;
        if (s[i] != '.' || parts == 4)
            return false;
        ++i;
    }
}

// An IPv4 address, or a hostname of at most kMaxHostChars (a trailing dot not
// counted) whose labels are 1..63 letters, digits and '-', not starting or
// ending with '-'. A name ending in an all-digit label must be an IPv4
// address ("999.1.1.1" is neither).
bool isValidHost(const std::string& host)
{
    if (isIpv4Literal(host))
        return true;
    std::string name = host;
    if (!name.empty() && name.back() == '.')
        name.pop_back();
    if (name.empty() || name.size() > kMaxHostChars)
        return false;
    std::size_t start = 0;
    bool last_all_digits = false;
    while (start <= name.size()) {
        std::size_t end = name.find('.', start);
        if (end == std::string::npos)
            end = name.size();
        const std::string label = name.substr(start, end - start);
        if (label.empty() || label.size() > kMaxLabelChars || label.front() == '-' || label.back() == '-')
            return false;
        if (!std::all_of(label.begin(), label.end(), [](char c) { return isAlnum(c) || c == '-'; }))
            return false;
        last_all_digits = std::all_of(label.begin(), label.end(), isDigit);
        start = end + 1;
    }
    return !last_all_digits;
}

bool isIpv4Multicast(const std::string& s)
{
    if (!isIpv4Literal(s))
        return false;
    const int first = std::stoi(s.substr(0, s.find('.')));
    return first >= 224 && first <= 239;
}

std::optional<InputTransport> parseTransport(const std::string& s)
{
    for (InputTransport t : {InputTransport::tcp_connect, InputTransport::udp, InputTransport::tcp_listen})
        if (s == transportName(t))
            return t;
    return std::nullopt;
}

namespace {

bool isNull(const JsonObject& o, const char* key)
{
    const auto text = o.getString(key);
    return text && *text == "null";
}

std::optional<uint8_t> getOctet(const JsonObject& o, const char* key)
{
    if (isNull(o, key))
        return std::nullopt;
    const auto value = o.getInt(key);
    if (!value || *value < 0 || *value > 255)
        fail(std::string(key) + " must be null or an integer from 0 to 255");
    return static_cast<uint8_t>(*value);
}

std::optional<InputType> parseType(const std::string& s)
{
    for (InputType t : {InputType::readsb, InputType::dump978, InputType::pingstation_json,
                        InputType::pingstation_cat021, InputType::beast_raw, InputType::uat_raw})  // never replay
        if (s == inputTypeName(t))
            return t;
    return std::nullopt;
}

bool isPingStation(InputType t) { return t == InputType::pingstation_json || t == InputType::pingstation_cat021; }
bool listens(InputTransport t) { return t != InputTransport::tcp_connect; }
bool takesCapture(InputType t) { return isPingStation(t) || t == InputType::beast_raw || t == InputType::uat_raw; }
InputTransport defaultTransport(InputType t)
{
    return t == InputType::pingstation_json ? InputTransport::udp : InputTransport::tcp_connect;
}

// Why `key` does not apply to an input of `type` over `transport`; "" when it does.
std::string notApplicable(const std::string& key, InputType type, InputTransport transport)
{
    if (key == "host")
        return listens(transport) ? std::string("host is not used with ") + transportName(transport) : "";
    if (key == "bind")
        return listens(transport) ? "" : std::string("bind is not used with ") + transportName(transport);
    if (key == "peer")
        return listens(transport) ? "" : "peer applies to udp and tcp_listen only";
    if (key == "group" || key == "interface")
        return transport == InputTransport::udp ? "" : key + " applies to udp only";
    if (key == "format")
        return type == InputType::beast_raw ? "" : "format applies to beast_raw only";
    if (key == "ref_lat" || key == "ref_lon")
        return type == InputType::beast_raw || type == InputType::uat_raw
                   ? ""
                   : key + " applies to beast_raw and uat_raw only";
    if (key == "source")
        return type == InputType::pingstation_json ? "" : "source applies to pingstation_json only";
    if (key == "capture")
        return takesCapture(type) ? ""
                                  : "capture applies to beast_raw, uat_raw, pingstation_json and pingstation_cat021 only";
    if (key == "edition" || key == "sac" || key == "sic")
        return type == InputType::pingstation_cat021 ? "" : key + " applies to pingstation_cat021 only";
    if (key == "status_url" || key == "status_interval_s" || key == "time_mode")
        return isPingStation(type) ? "" : key + " applies to pingstation_json and pingstation_cat021 only";
    return "";
}

void appendOptionalOctet(JsonWriter& w, const char* key, std::optional<uint8_t> v)
{
    if (v)
        w.add(key, static_cast<int64_t>(*v));
    else
        w.addRaw(key, "null");
}

}  // namespace

const char* inputTypeName(InputType type)
{
    switch (type) {
    case InputType::readsb: return "readsb";
    case InputType::dump978: return "dump978";
    case InputType::pingstation_json: return "pingstation_json";
    case InputType::pingstation_cat021: return "pingstation_cat021";
    case InputType::beast_raw: return "beast_raw";
    case InputType::uat_raw: return "uat_raw";
    case InputType::replay: return "replay";
    }
    return "?";
}

const char* transportName(InputTransport transport)
{
    switch (transport) {
    case InputTransport::tcp_connect: return "tcp_connect";
    case InputTransport::udp: return "udp";
    case InputTransport::tcp_listen: return "tcp_listen";
    }
    return "?";
}

bool isTrackType(InputType type)
{
    return type != InputType::dump978 && type != InputType::beast_raw && type != InputType::uat_raw;
}

bool InputDef::operator==(const InputDef& o) const
{
    return id == o.id && name == o.name && type == o.type && enabled == o.enabled && transport == o.transport &&
           bind == o.bind && host == o.host && port == o.port && edition == o.edition && sac == o.sac &&
           sic == o.sic && status_url == o.status_url && status_interval_s == o.status_interval_s &&
           time_mode == o.time_mode && capture == o.capture && peer == o.peer && builtin == o.builtin &&
           group == o.group && iface == o.iface && format == o.format && ref_lat == o.ref_lat &&
           ref_lon == o.ref_lon && source == o.source;
}

InputDef parseInputDef(const JsonObject& o, NameRule rule, bool* repaired)
{
    std::set<std::string> seen;
    for (const auto& key : o.keys()) {
        if (std::find(std::begin(kKeys), std::end(kKeys), key) == std::end(kKeys))
            fail("unknown key '" + clipForMessage(key) + "'");
        if (!seen.insert(key).second)
            fail("duplicate key '" + clipForMessage(key) + "'");
    }

    InputDef d;
    if (!o.has("id"))
        fail("id is required");
    d.id = json_get::text<InputDefError>(o, "id", "a string");
    if (!isValidInputId(d.id))
        fail("id must be 1 to 32 characters of a-z, 0-9 and -");
    for (const char* reserved : kReservedIds)
        if (d.id == reserved)
            fail("id '" + d.id + "' is reserved");

    const char* const kTypeRule =
        "type must be one of readsb, dump978, pingstation_json, pingstation_cat021, beast_raw, uat_raw";
    if (!o.has("type"))
        fail("type is required");
    const auto type = parseType(json_get::text<InputDefError>(o, "type", "a string"));
    if (!type)
        fail(kTypeRule);
    d.type = *type;
    d.transport = defaultTransport(d.type);
    if (d.type == InputType::pingstation_cat021 && !o.has("transport"))
        fail("transport is required for pingstation_cat021 (udp, tcp_connect or tcp_listen)");
    if (o.has("transport")) {
        const auto transport = parseTransport(json_get::text<InputDefError>(o, "transport", "udp, tcp_connect or tcp_listen"));
        if (!transport)
            fail("transport must be udp, tcp_connect or tcp_listen");
        d.transport = *transport;
    }
    for (const auto& key : o.keys()) {
        const std::string why = notApplicable(key, d.type, d.transport);
        if (!why.empty())
            fail(why);
    }

    if (o.has("name")) {
        d.name = json_get::text<InputDefError>(o, "name", "a string");
        if (!isValidInputName(d.name)) {
            if (rule != NameRule::repair)
                fail("name must be at most 64 printable characters");
            d.name = repairInputName(d.name);
            if (repaired != nullptr)
                *repaired = true;
        }
    }
    if (o.has("enabled"))
        d.enabled = json_get::flag<InputDefError>(o, "enabled");
    if (!o.has("port"))
        fail("port is required");
    d.port = static_cast<uint16_t>(json_get::intIn<InputDefError>(o, "port", 1, 65535));
    if (listens(d.transport)) {
        if (d.transport == InputTransport::udp) {
            d.group = json_get::optionalText<InputDefError>(o, "group", "a multicast address");
            if (!d.group.empty() && !isIpv4Multicast(d.group))
                fail("group must be a multicast address (224.0.0.0 to 239.255.255.255)");
            d.iface = json_get::optionalText<InputDefError>(o, "interface", "an IPv4 address");
            if (!d.iface.empty()) {
                if (d.group.empty())
                    fail("interface applies to a multicast group only");
                if (!isIpv4Literal(d.iface))
                    fail("interface must be an IPv4 address");
            }
        }
        if (o.has("bind")) {
            if (!d.group.empty())
                fail("bind does not apply when group is set");
            d.bind = json_get::text<InputDefError>(o, "bind", "an IPv4 address");
            if (!isIpv4Literal(d.bind))
                fail("bind must be an IPv4 address");
        }
        d.peer = json_get::optionalText<InputDefError>(o, "peer", "an IPv4 address");
        if (!d.peer.empty() && !isIpv4Literal(d.peer))
            fail("peer must be an IPv4 address");
    } else {
        if (!o.has("host"))
            fail("host is required");
        d.host = json_get::text<InputDefError>(o, "host", "a string");
        if (!isValidHost(d.host))
            fail("host must be an IPv4 address or a hostname of at most 253 characters");
    }
    if (d.type == InputType::pingstation_cat021) {
        if (o.has("edition")) {
            d.edition = json_get::text<InputDefError>(o, "edition", "a string");
            if (std::find(std::begin(kEditions), std::end(kEditions), d.edition) == std::end(kEditions))
                fail("edition must be one of 2.2, 2.3, 2.4, 2.5, 2.6, 2.7");
        }
        if (o.has("sac"))
            d.sac = getOctet(o, "sac");
        if (o.has("sic"))
            d.sic = getOctet(o, "sic");
    }
    if (isPingStation(d.type)) {
        d.status_url = json_get::optionalText<InputDefError>(o, "status_url", "a string");
        if (!d.status_url.empty() && !parseHttpUrl(d.status_url))
            fail("status_url must be an http:// URL without userinfo, with a port of 1 to 65535");
        if (o.has("status_interval_s"))
            d.status_interval_s =
                static_cast<int>(json_get::intIn<InputDefError>(o, "status_interval_s", kMinStatusInterval, kMaxStatusInterval));
        if (o.has("time_mode")) {
            const std::string mode = json_get::text<InputDefError>(o, "time_mode", "utc or gps");
            if (mode == "utc")
                d.time_mode = TimeMode::utc;
            else if (mode == "gps")
                d.time_mode = TimeMode::gps;
            else
                fail("time_mode must be utc or gps");
        }
    }
    if (d.type == InputType::pingstation_json && o.has("source")) {
        const std::string source = json_get::text<InputDefError>(o, "source", "device or decode");
        if (source == "device")
            d.source = PingSource::device;
        else if (source == "decode")
            d.source = PingSource::decode;
        else
            fail("source must be device or decode");
    }
    if (takesCapture(d.type) && o.has("capture"))
        d.capture = json_get::flag<InputDefError>(o, "capture");
    if (d.type == InputType::beast_raw && o.has("format")) {
        d.format = json_get::text<InputDefError>(o, "format", "beast or avr");
        if (d.format != "beast" && d.format != "avr")
            fail("format must be beast or avr");
    }
    // The reference position for decoding (spec 8.1): both or neither.
    const bool has_lat = o.has("ref_lat") && !isNull(o, "ref_lat");
    const bool has_lon = o.has("ref_lon") && !isNull(o, "ref_lon");
    if (has_lat != has_lon)
        fail("ref_lat and ref_lon must be set together");
    if (has_lat) {
        const auto lat = o.getDouble("ref_lat");
        if (!lat || *lat < -90 || *lat > 90)
            fail("ref_lat must be a number from -90 to 90");
        const auto lon = o.getDouble("ref_lon");
        if (!lon || *lon < -180 || *lon > 180)
            fail("ref_lon must be a number from -180 to 180");
        d.ref_lat = lat;
        d.ref_lon = lon;
    }
    return d;
}

std::string inputDefJson(const InputDef& d)
{
    JsonWriter w;
    w.add("id", d.id).add("name", d.name).add("type", inputTypeName(d.type)).add("enabled", d.enabled);
    if (d.type == InputType::replay)
        return w.str();
    if (d.type == InputType::pingstation_cat021 || d.type == InputType::beast_raw || d.type == InputType::uat_raw ||
        d.transport != defaultTransport(d.type))
        w.add("transport", transportName(d.transport));
    if (!listens(d.transport))
        w.add("host", d.host);
    else if (d.group.empty())
        w.add("bind", d.bind);
    w.add("port", static_cast<int64_t>(d.port));
    if (listens(d.transport))
        w.addOptionalText("peer", d.peer);
    if (!d.group.empty()) {
        w.add("group", d.group);
        w.addOptionalText("interface", d.iface);
    }
    if (d.type == InputType::beast_raw)
        w.add("format", d.format);
    if (d.ref_lat && d.ref_lon)
        w.addExact("ref_lat", *d.ref_lat).addExact("ref_lon", *d.ref_lon);
    if (d.type == InputType::pingstation_cat021) {
        w.add("edition", d.edition);
        appendOptionalOctet(w, "sac", d.sac);
        appendOptionalOctet(w, "sic", d.sic);
    }
    if (isPingStation(d.type)) {
        w.addOptionalText("status_url", d.status_url);
        w.add("status_interval_s", static_cast<int64_t>(d.status_interval_s));
        w.add("time_mode", d.time_mode == TimeMode::gps ? "gps" : "utc");
    }
    if (d.type == InputType::pingstation_json && d.source == PingSource::decode)
        w.add("source", "decode");  // the default is not written, so a v1 file stays readable by the previous release
    if (takesCapture(d.type))
        w.add("capture", d.capture);
    return w.str();
}

std::optional<HttpUrl> parseHttpUrl(const std::string& url)
{
    const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0)
        return std::nullopt;
    for (char c : url) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7f || c == '#')
            return std::nullopt;
    }
    const std::size_t begin = scheme.size();
    std::size_t end = url.find_first_of("/?", begin);
    if (end == std::string::npos)
        end = url.size();
    const std::string authority = url.substr(begin, end - begin);
    if (authority.find('@') != std::string::npos)
        return std::nullopt;

    HttpUrl out;
    out.host = authority;
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        out.host = authority.substr(0, colon);
        const std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), isDigit))
            return std::nullopt;
        const long value = std::stol(port);
        if (value < 1 || value > 65535)
            return std::nullopt;
        out.port = static_cast<uint16_t>(value);
    }
    if (!isValidHost(out.host))
        return std::nullopt;
    if (end < url.size())
        out.target = url[end] == '/' ? url.substr(end) : "/" + url.substr(end);
    return out;
}

std::string repairInputName(const std::string& name)
{
    std::string out;
    std::size_t chars = 0;
    for (std::size_t i = 0; i < name.size() && chars < kMaxNameChars;) {
        std::uint32_t cp = 0;
        const std::size_t len = utf8At(name, i, cp);
        if (len == 0) {
            out += "\xEF\xBF\xBD";  // U+FFFD, one per bad byte
            ++i;
            ++chars;
            continue;
        }
        if (!(cp < 0x20 || (cp >= 0x7F && cp <= 0x9F))) {
            out.append(name, i, len);
            ++chars;
        }
        i += len;
    }
    return out;
}

std::string clipForMessage(const std::string& text)
{
    constexpr std::size_t kMax = 64;
    if (text.size() <= kMax)
        return text;
    std::size_t end = kMax;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;  // text[end] continues a character: cut before that character
    return text.substr(0, end) + "...";
}

}  // namespace skycued
