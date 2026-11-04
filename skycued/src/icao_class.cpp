#include "icao_class.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace skycued {

namespace {

struct IcaoBlock {
    uint32_t first;
    uint32_t last;
    const char* name;
};
struct MilitaryBlock {
    uint32_t first;
    uint32_t last;
};

#include "icao_tables.inc"

template <typename T, std::size_t N>
constexpr std::size_t countOf(const T (&)[N])
{
    return N;
}

// app_pure.js parseHex: exactly 6 hex digits, either case.
std::optional<uint32_t> parseHex(const std::string& hex)
{
    if (hex.size() != 6)
        return std::nullopt;
    uint32_t v = 0;
    for (char c : hex) {
        uint32_t d;
        if (c >= '0' && c <= '9')
            d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = static_cast<uint32_t>(c - 'A' + 10);
        else
            return std::nullopt;
        v = (v << 4) | d;
    }
    return v;
}

// The block holding v in a table sorted by first address, or nullptr.
template <typename Block, std::size_t N>
const Block* findBlock(const Block (&table)[N], uint32_t v)
{
    std::size_t lo = 0, hi = N;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (v < table[mid].first)
            hi = mid;
        else if (v > table[mid].last)
            lo = mid + 1;
        else
            return &table[mid];
    }
    return nullptr;
}

bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// /^[A-Z]{3}[A-Z0-9]{1,4}$/ with a digit after the first three.
bool airlineLike(const std::string& s)
{
    if (s.size() < 4 || s.size() > 7)
        return false;
    bool digit = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i < 3 ? !isUpper(s[i]) : !(isUpper(s[i]) || isDigit(s[i])))
            return false;
        if (i >= 3 && isDigit(s[i]))
            digit = true;
    }
    return digit;
}

// /^N[1-9]\d{0,4}[A-Z]{0,2}$/
bool nNumber(const std::string& s)
{
    if (s.size() < 2 || s[0] != 'N' || s[1] < '1' || s[1] > '9')
        return false;
    std::size_t i = 2, digits = 0, letters = 0;
    while (i < s.size() && isDigit(s[i]) && digits < 4) {
        ++i;
        ++digits;
    }
    while (i < s.size() && isUpper(s[i]) && letters < 2) {
        ++i;
        ++letters;
    }
    return i == s.size();
}

// /^[A-Z]{4,6}$/
bool lettersOnly(const std::string& s)
{
    if (s.size() < 4 || s.size() > 6)
        return false;
    for (char c : s)
        if (!isUpper(c))
            return false;
    return true;
}

}  // namespace

std::optional<std::string> icaoCountry(const std::string& hex)
{
    const auto v = parseHex(hex);
    if (!v)
        return std::nullopt;
    const IcaoBlock* b = findBlock(kIcaoBlocks, *v);
    if (!b)
        return std::nullopt;
    return std::string(b->name);
}

bool icaoMilitary(const std::string& hex)
{
    const auto v = parseHex(hex);
    return v && findBlock(kMilitaryBlocks, *v) != nullptr;
}

CallsignKind callsignKind(const std::string& callsign)
{
    std::size_t b = 0, e = callsign.size();
    while (b < e && isSpace(callsign[b]))
        ++b;
    while (e > b && isSpace(callsign[e - 1]))
        --e;
    std::string s = callsign.substr(b, e - b);
    if (s.empty())
        return CallsignKind::none;
    for (char& c : s)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    for (std::size_t i = 0; i < countOf(kMilitaryCallsignPrefixes); ++i) {
        const char* p = kMilitaryCallsignPrefixes[i];
        const std::size_t n = std::strlen(p);
        if (s.size() > n && s.compare(0, n, p) == 0)
            return CallsignKind::military;
    }
    if (airlineLike(s))
        return CallsignKind::airline;
    if (nNumber(s) || lettersOnly(s))
        return CallsignKind::registration;
    return CallsignKind::other;
}

std::optional<std::string> trackCountry(const std::string& hex, skycue::AddressType addr_type)
{
    if (addr_type != skycue::AddressType::icao)
        return std::nullopt;
    return icaoCountry(hex);
}

const char* trackClass(const ClassInput& t)
{
    const CallsignKind kind = callsignKind(t.callsign);
    const bool icao = t.addr_type == skycue::AddressType::icao;
    if ((icao && icaoMilitary(t.hex)) || kind == CallsignKind::military)
        return "military";
    const std::string& cat = t.category;
    if (kind == CallsignKind::airline || (kind != CallsignKind::none && (cat == "A3" || cat == "A4" || cat == "A5")))
        return "airline";
    if (cat == "A1" || cat == "A2" || cat == "A7" || kind == CallsignKind::registration ||
        (!t.registration.empty() && kind == CallsignKind::none))
        return "private";
    return "unknown";
}

}  // namespace skycued
