#include "asterix/capture_line.hpp"

#include "json_nesting.hpp"
#include "time_util.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <cstdio>
#include <sstream>

namespace skycued {

namespace {

constexpr int kMaxCaptureLineDepth = 32;

// Ids and transports are plain ASCII by their own rules; escape anyway so a
// line is always valid JSON.
void appendJsonString(std::string& out, const std::string& s)
{
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += ch;
        }
    }
    out += '"';
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// The string value of a top-level key; nullopt when missing or an object/array.
std::optional<std::string> stringKey(const boost::property_tree::ptree& tree, const char* key)
{
    const auto child = tree.get_child_optional(key);
    if (!child || !child->empty())
        return std::nullopt;
    return child->data();
}

}  // namespace

std::string captureLineJson(const CaptureLine& line)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(80 + line.input.size() + 2 * line.bytes.size());
    out += "{\"t\":";
    appendJsonString(out, formatIso8601Ms(line.t));
    out += ",\"input\":";
    appendJsonString(out, line.input);
    out += ",\"transport\":";
    appendJsonString(out, line.transport);
    out += ",\"hex\":\"";
    for (const uint8_t b : line.bytes) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0x0F];
    }
    out += "\"}";
    return out;
}

std::optional<CaptureLine> parseCaptureLine(const std::string& text)
{
    // A line holds no nesting at all; refuse deep input before read_json
    // recurses into it (the same guard and limit as JsonObject::parse).
    if (!nestingWithin(text, kMaxCaptureLineDepth))
        return std::nullopt;
    boost::property_tree::ptree tree;
    try {
        std::istringstream in(text);
        boost::property_tree::read_json(in, tree);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    const auto t = stringKey(tree, "t"), input = stringKey(tree, "input"), transport = stringKey(tree, "transport"),
               hex = stringKey(tree, "hex");
    if (!t || !input || !transport || !hex || input->empty())
        return std::nullopt;
    if (*transport != "udp" && *transport != "tcp_connect" && *transport != "tcp_listen")
        return std::nullopt;
    const auto when = parseIso8601Ms(*t);
    if (!when || hex->size() % 2 != 0)
        return std::nullopt;
    CaptureLine line;
    line.t = *when;
    line.input = *input;
    line.transport = *transport;
    line.bytes.reserve(hex->size() / 2);
    for (std::size_t i = 0; i < hex->size(); i += 2) {
        const int hi = hexValue((*hex)[i]), lo = hexValue((*hex)[i + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        line.bytes.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return line;
}

}  // namespace skycued
