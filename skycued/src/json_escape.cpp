#include "json_escape.hpp"

#include <cstddef>
#include <cstdio>

namespace skycued {

namespace {

// The length of the well-formed UTF-8 sequence (RFC 3629) starting at p[i],
// 0 when there is none: a continuation byte, 0xc0/0xc1, 0xf5..0xff, a bad or
// missing following byte, an overlong form, a surrogate or a value above
// U+10FFFF.
std::size_t utf8SequenceLength(const unsigned char* p, std::size_t i, std::size_t n)
{
    const unsigned char c = p[i];
    std::size_t len;
    unsigned char lo = 0x80, hi = 0xbf;  // bounds of the second byte
    if (c >= 0xc2 && c <= 0xdf) {
        len = 2;
    } else if (c >= 0xe0 && c <= 0xef) {
        len = 3;
        if (c == 0xe0)
            lo = 0xa0;  // overlong
        else if (c == 0xed)
            hi = 0x9f;  // surrogates
    } else if (c >= 0xf0 && c <= 0xf4) {
        len = 4;
        if (c == 0xf0)
            lo = 0x90;  // overlong
        else if (c == 0xf4)
            hi = 0x8f;  // above U+10FFFF
    } else {
        return 0;
    }
    if (n - i < len)
        return 0;
    if (p[i + 1] < lo || p[i + 1] > hi)
        return 0;
    for (std::size_t k = 2; k < len; ++k)
        if (p[i + k] < 0x80 || p[i + k] > 0xbf)
            return 0;
    return len;
}

}  // namespace

bool isValidUtf8(const std::string& s)
{
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    for (std::size_t i = 0; i < n;) {
        if (p[i] < 0x80) {
            ++i;
            continue;
        }
        const std::size_t len = utf8SequenceLength(p, i, n);
        if (len == 0)
            return false;
        i += len;
    }
    return true;
}

void appendJsonString(std::string& out, const std::string& s)
{
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    out += '"';
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c >= 0x80) {
            const std::size_t len = utf8SequenceLength(p, i, n);
            if (len == 0) {
                out += "\xEF\xBF\xBD";  // U+FFFD, one per bad byte
                ++i;
            } else {
                out.append(s, i, len);
                i += len;
            }
            continue;
        }
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                // NOLINTNEXTLINE(cert-err33-c): bounded write into a local buffer sized for the format
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
        ++i;
    }
    out += '"';
}

}  // namespace skycued
