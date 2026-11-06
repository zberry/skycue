#include "conf_rewrite.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace skycued {
namespace conf_rewrite {

namespace {

// The unquoted value characters of the grammar (config.cpp).
bool isPlainChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           std::strchr("_.,:/+@%=-", c) != nullptr;
}

}  // namespace

std::string quoteValue(const std::string& value)
{
    if (value.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("a value cannot hold a line break");
    if (std::all_of(value.begin(), value.end(), isPlainChar))
        return value;
    if (value.find_first_of("\"$`\\") == std::string::npos)
        return "\"" + value + "\"";
    if (value.find('\'') == std::string::npos)
        return "'" + value + "'";
    throw std::invalid_argument("value '" + value + "' cannot be written in skycue.conf (it holds both \" and ')");
}

std::string rewriteKeys(const std::string& text, const std::map<std::string, std::string>& values)
{
    std::map<std::string, std::string> quoted;
    for (const auto& kv : values)
        quoted[kv.first] = quoteValue(kv.second);

    const size_t first_nl = text.find('\n');
    const std::string eol = first_nl != std::string::npos && first_nl > 0 && text[first_nl - 1] == '\r' ? "\r\n" : "\n";

    std::string out;
    out.reserve(text.size() + 64);
    std::set<std::string> seen;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        size_t end = nl == std::string::npos ? text.size() : nl + 1;  // past the line break
        size_t content_end = nl == std::string::npos ? text.size() : nl;
        if (content_end > pos && text[content_end - 1] == '\r')
            --content_end;
        const size_t eq = text.find('=', pos);
        bool replaced = false;
        if (eq != std::string::npos && eq < content_end) {
            auto it = quoted.find(text.substr(pos, eq - pos));
            if (it != quoted.end()) {
                out.append(text, pos, eq + 1 - pos);
                out += it->second;
                out.append(text, content_end, end - content_end);
                seen.insert(it->first);
                replaced = true;
            }
        }
        if (!replaced)
            out.append(text, pos, end - pos);
        pos = end;
    }

    bool header = false;
    for (const auto& kv : quoted) {
        if (seen.count(kv.first))
            continue;
        if (!header) {
            if (!out.empty() && out.back() != '\n')
                out += eol;
            out += "# added by skycue save-baseline" + eol;
            header = true;
        }
        out += kv.first + "=" + kv.second + eol;
    }
    return out;
}

}  // namespace conf_rewrite
}  // namespace skycued
