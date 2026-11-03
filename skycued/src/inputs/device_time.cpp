#include "inputs/device_time.hpp"

#include <cmath>

namespace skycued {
namespace {

using std::chrono::system_clock;

constexpr int64_t kSecondsPerDay = 86400;
constexpr int64_t kGpsEpochUnixS = 315964800;

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

bool leapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int daysInMonth(int y, int m)
{
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leapYear(y) ? 29 : kDays[m - 1];
}

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's
// days_from_civil).
int64_t daysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

system_clock::time_point fromNs(int64_t ns)
{
    return system_clock::time_point(std::chrono::duration_cast<system_clock::duration>(std::chrono::nanoseconds(ns)));
}

}  // namespace

std::optional<system_clock::time_point> parseDeviceTime(const std::string& s)
{
    // YYYY-MM-DDTHH:MM:SS is 19 characters.
    if (s.size() < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':')
        return std::nullopt;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (!digits(s, 0, 4, y) || !digits(s, 5, 2, mo) || !digits(s, 8, 2, d) || !digits(s, 11, 2, h) ||
        !digits(s, 14, 2, mi) || !digits(s, 17, 2, sec))
        return std::nullopt;
    if (mo < 1 || mo > 12 || d < 1 || d > daysInMonth(y, mo) || h > 23 || mi > 59 || sec > 59)
        return std::nullopt;

    std::size_t pos = 19;
    int64_t frac_ns = 0;
    if (s[pos] == '.' || s[pos] == ':') {
        ++pos;
        std::size_t n = 0;
        while (pos + n < s.size() && s[pos + n] >= '0' && s[pos + n] <= '9')
            ++n;
        if (n < 1 || n > 9)
            return std::nullopt;
        int v = 0;
        digits(s, pos, n, v);
        frac_ns = v;
        for (std::size_t i = n; i < 9; ++i)
            frac_ns *= 10;
        pos += n;
    }
    if (pos + 1 != s.size() || s[pos] != 'Z')
        return std::nullopt;

    const int64_t secs = daysFromCivil(y, mo, d) * kSecondsPerDay + h * 3600 + mi * 60 + sec;
    return fromNs(secs * 1000000000 + frac_ns);
}

ClockCheck checkDeviceTime(std::optional<system_clock::time_point> device, system_clock::time_point arrival,
                           TimeMode mode)
{
    ClockCheck out{arrival, std::nullopt, false};
    if (!device)
        return out;
    system_clock::time_point t = *device;
    if (mode == TimeMode::gps)
        t -= kGpsUtcOffset;
    const auto offset = t - arrival;
    out.offset_s = std::chrono::duration<double>(offset).count();
    if (offset > kMaxDeviceSkew || offset < -kMaxDeviceSkew)
        out.rejected = true;
    else
        out.used = t;
    return out;
}

std::optional<system_clock::time_point> timeOfDay(double tod_s, system_clock::time_point now)
{
    if (!(tod_s >= -1.0 && tod_s < kSecondsPerDay + 2.0))  // false for NaN too
        return std::nullopt;
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    const int64_t day_ns = kSecondsPerDay * 1000000000;
    int64_t midnight_ns = now_ns / day_ns * day_ns;
    if (now_ns < 0 && midnight_ns != now_ns)
        midnight_ns -= day_ns;  // floor, for times before 1970
    const double now_tod_s = static_cast<double>(now_ns - midnight_ns) / 1e9;
    const double half_day_s = kSecondsPerDay / 2.0;
    if (tod_s - now_tod_s > half_day_s)
        midnight_ns -= day_ns;
    else if (tod_s - now_tod_s < -half_day_s)
        midnight_ns += day_ns;
    return fromNs(midnight_ns + std::llround(tod_s * 1e9));
}

std::optional<system_clock::time_point> gpsCounterTime(uint64_t s, uint64_t ns)
{
    if (s >= kMaxGpsCounterS || ns >= 1000000000)
        return std::nullopt;
    return fromNs((kGpsEpochUnixS + static_cast<int64_t>(s)) * 1000000000 + static_cast<int64_t>(ns));
}

}  // namespace skycued
