#include "json_lite.hpp"

#include <boost/property_tree/json_parser.hpp>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <locale>
#include <sstream>

namespace skycued {
namespace {

// PropertyTree's parser recurses once per nesting level, so deep input would
// overflow the stack (a crash, not an exception). This linear scan finds the
// maximum {/[ depth outside string literals so such input is refused first.
bool nestingWithin(const std::string& text, int max_depth)
{
    int depth = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (c == '\\')
                ++i;  // skip the escaped character, so \" does not end the string
            else if (c == '"')
                in_string = false;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            if (++depth > max_depth)
                return false;
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return true;
}

}  // namespace

std::optional<JsonObject> JsonObject::parse(const std::string& text)
{
    if (text.empty() || text.size() > kMaxTextBytes || !nestingWithin(text, kMaxJsonDepth))
        return std::nullopt;
    boost::property_tree::ptree tree;
    try {
        std::istringstream in(text);
        boost::property_tree::read_json(in, tree);
    } catch (const std::exception&) {  // json_parser_error, or bad_alloc
        return std::nullopt;
    }
    // A top-level scalar leaves its text in the root; an array gives children
    // with empty keys. Neither is an object.
    if (!tree.data().empty())
        return std::nullopt;
    for (const auto& child : tree)
        if (child.first.empty())
            return std::nullopt;
    return JsonObject(std::move(tree));
}

const boost::property_tree::ptree* JsonObject::scalar(const char* key) const
{
    auto it = tree_.find(key);
    if (it == tree_.not_found() || !it->second.empty())  // absent, or an array/object
        return nullptr;
    return &it->second;
}

bool JsonObject::has(const char* key) const { return tree_.find(key) != tree_.not_found(); }

std::optional<std::string> JsonObject::getString(const char* key) const
{
    const auto* node = scalar(key);
    if (!node)
        return std::nullopt;
    return node->data();
}

std::optional<double> JsonObject::getDouble(const char* key) const
{
    const auto* node = scalar(key);
    if (!node || node->data().empty())
        return std::nullopt;
    // std::stod/strtod follow LC_NUMERIC; a classic-locale stream does not.
    std::istringstream in(node->data());
    in.imbue(std::locale::classic());
    double value = 0;
    in >> std::noskipws >> value;
    if (in.fail() || in.get() != std::char_traits<char>::eof() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

std::optional<int64_t> JsonObject::getInt(const char* key) const
{
    const auto* node = scalar(key);
    if (!node)
        return std::nullopt;
    const std::string& s = node->data();
    // Only the integer from_chars overload is used: libstdc++ has it since
    // gcc 8.1 (the floating-point overloads came in gcc 11).
    int64_t value = 0;
    const char* end = s.data() + s.size();
    auto result = std::from_chars(s.data(), end, value);
    if (s.empty() || result.ec != std::errc() || result.ptr != end)
        return std::nullopt;
    return value;
}

void appendJsonString(std::string& out, const std::string& s)
{
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void JsonWriter::key(const char* k)
{
    if (!body_.empty())
        body_ += ',';
    appendJsonString(body_, k);
    body_ += ':';
}

JsonWriter& JsonWriter::add(const char* k, const std::string& value)
{
    key(k);
    appendJsonString(body_, value);
    return *this;
}

JsonWriter& JsonWriter::add(const char* k, double value)
{
    key(k);
    if (!std::isfinite(value)) {
        body_ += "null";
        return *this;
    }
    // Same output as printf("%.7g"), but always with a '.' decimal point.
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(7);
    out << value;
    body_ += out.str();
    return *this;
}

JsonWriter& JsonWriter::add(const char* k, int64_t value)
{
    key(k);
    char buf[24];
    auto result = std::to_chars(buf, buf + sizeof buf, value);
    body_.append(buf, result.ptr);
    return *this;
}

JsonWriter& JsonWriter::add(const char* k, bool value)
{
    key(k);
    body_ += value ? "true" : "false";
    return *this;
}

std::string JsonWriter::str() const { return "{" + body_ + "}"; }

}  // namespace skycued
