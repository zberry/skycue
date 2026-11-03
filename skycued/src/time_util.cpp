#include "time_util.hpp"

#include <cstdio>
#include <ctime>

namespace skycued {
namespace {

using std::chrono::milliseconds;
using std::chrono::system_clock;

// Splits a millisecond count into whole seconds and a 0..999 remainder
// (floor division, so times before 1970 keep a positive fraction).
void splitMs(int64_t ms, int64_t& secs, int& frac)
{
    secs = ms / 1000;
    int64_t rem = ms % 1000;
    if (rem < 0) {
        rem += 1000;
        --secs;
    }
    frac = static_cast<int>(rem);
}

std::tm utc(int64_t secs)
{
    std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    return tm;
}

bool digits(const std::string& s, std::size_t pos, std::size_t n, int& out)
{
    if (pos + n > s.size())
        return false;
    int v = 0;
    for (std::size_t i = pos; i < pos + n; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (s[i] - '0');
    }
    out = v;
    return true;
}

}  // namespace

std::string formatIso8601Ms(system_clock::time_point when)
{
    const int64_t ms = std::chrono::round<milliseconds>(when.time_since_epoch()).count();
    int64_t secs = 0;
    int frac = 0;
    splitMs(ms, secs, frac);
    const std::tm tm = utc(secs);
    char buf[80];  // room for any int fields: gcc 8 -Wformat-truncation
    // NOLINTNEXTLINE(cert-err33-c): bounded write into a local buffer sized for the format
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, frac);
    return buf;
}

std::optional<system_clock::time_point> parseIso8601Ms(const std::string& s)
{
    // YYYY-MM-DDTHH:MM:SS = 19 characters, then optional .f{1,3}, then Z.
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (s.size() < 20 || !digits(s, 0, 4, year) || s[4] != '-' || !digits(s, 5, 2, month) || s[7] != '-' ||
        !digits(s, 8, 2, day) || s[10] != 'T' || !digits(s, 11, 2, hour) || s[13] != ':' ||
        !digits(s, 14, 2, minute) || s[16] != ':' || !digits(s, 17, 2, second))
        return std::nullopt;
    std::size_t pos = 19;
    int frac_ms = 0;
    if (s[pos] == '.') {
        ++pos;
        std::size_t n = 0;
        while (pos + n < s.size() && s[pos + n] >= '0' && s[pos + n] <= '9')
            ++n;
        if (n < 1 || n > 3 || !digits(s, pos, n, frac_ms))
            return std::nullopt;
        for (std::size_t i = n; i < 3; ++i)
            frac_ms *= 10;
        pos += n;
    }
    if (pos + 1 != s.size() || s[pos] != 'Z')
        return std::nullopt;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
        return std::nullopt;

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    const std::time_t t = timegm(&tm);
    // timegm normalises out-of-range days (Feb 31 -> Mar 3); reject those.
    if (tm.tm_mday != day || tm.tm_mon != month - 1)
        return std::nullopt;
    // system_clock counts int64 nanoseconds (1677-09-21 to 2262-04-11); a
    // time outside would overflow the conversion. 4 digits of year keep the
    // milliseconds themselves far inside int64_t.
    constexpr int64_t kMaxMs = std::chrono::duration_cast<milliseconds>(system_clock::duration::max()).count();
    const int64_t ms = static_cast<int64_t>(t) * 1000 + frac_ms;
    if (ms > kMaxMs || ms < -kMaxMs)
        return std::nullopt;
    return system_clock::time_point(std::chrono::duration_cast<system_clock::duration>(milliseconds(ms)));
}

std::string formatCompactUtc(system_clock::time_point when)
{
    int64_t secs = 0;
    int frac = 0;
    splitMs(std::chrono::duration_cast<milliseconds>(when.time_since_epoch()).count(), secs, frac);
    const std::tm tm = utc(secs);
    char buf[32];
    // NOLINTNEXTLINE(cert-err33-c): bounded write into a local buffer sized for the format
    std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string hexAddress(uint32_t address)
{
    char buf[8];
    // NOLINTNEXTLINE(cert-err33-c): bounded write into a local buffer sized for the format
    std::snprintf(buf, sizeof buf, "%06x", static_cast<unsigned>(address & 0xffffffu));
    return buf;
}

std::optional<uint32_t> parseHexAddress(const std::string& text)
{
    if (text.size() != 6)
        return std::nullopt;
    uint32_t value = 0;
    for (char c : text) {
        uint32_t digit;
        if (c >= '0' && c <= '9')
            digit = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = static_cast<uint32_t>(c - 'A' + 10);
        else
            return std::nullopt;
        value = (value << 4) | digit;
    }
    return value;
}

}  // namespace skycued
