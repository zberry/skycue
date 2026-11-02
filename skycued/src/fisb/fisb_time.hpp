#pragma once

// Partial UTC times (hh:mm, dd hh:mm, mm/dd hh:mm) resolved against a
// reference time, the way docs/fisb-reference.md §7 describes: fill the
// missing fields from the reference, try the neighbouring day, month or year
// as well, and keep the candidate closest to the reference (the earlier one
// on a tie). Header-only; no time zones, no std::tm.

#include "fisb/apdu.hpp"
#include "fisb/products.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace skycued {
namespace fisb {

namespace detail {

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's
// days_from_civil, public domain).
constexpr int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

struct Civil {
    int64_t y;
    unsigned m, d;
};

// Inverse of daysFromCivil (civil_from_days).
constexpr Civil civilFromDays(int64_t z)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return {static_cast<int64_t>(yoe) + era * 400 + (m <= 2), m, d};
}

constexpr unsigned daysInMonth(int64_t y, unsigned m)
{
    if (m == 2)
        return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 29u : 28u;
    return (m == 4 || m == 6 || m == 9 || m == 11) ? 30u : 31u;
}

inline std::optional<TimePoint> makeUtc(int64_t y, int64_t m, unsigned d, unsigned hh, unsigned mm)
{
    // Normalise a month outside 1..12 into the neighbouring year.
    while (m < 1) {
        m += 12;
        --y;
    }
    while (m > 12) {
        m -= 12;
        ++y;
    }
    const auto um = static_cast<unsigned>(m);
    if (d < 1 || d > daysInMonth(y, um))
        return std::nullopt;
    const int64_t days = daysFromCivil(y, um, d);
    const int64_t secs = days * 86400 + static_cast<int64_t>(hh) * 3600 + static_cast<int64_t>(mm) * 60;
    return TimePoint(std::chrono::duration_cast<TimePoint::duration>(std::chrono::seconds(secs)));
}

}  // namespace detail

// The UTC time with the given fields closest to ref. month and day are
// optional (missing ones come from ref; with only hh:mm the day is ref's day
// -1/0/+1, with dd hh:mm the month is ref's -1/0/+1, with mm/dd hh:mm the
// year is ref's -1/0/+1). An hour of 24 means 00 of the next day. nullopt
// when a field is out of range or no candidate date exists (e.g. 02/30).
inline std::optional<TimePoint> resolveUtc(TimePoint ref, std::optional<unsigned> month, std::optional<unsigned> day,
                                           unsigned hour, unsigned minute)
{
    if (hour > 24 || minute > 59 || (hour == 24 && minute != 0))
        return std::nullopt;
    if (month && (*month < 1 || *month > 12))
        return std::nullopt;
    if (month && !day)
        return std::nullopt;
    if (day && (*day < 1 || *day > 31))
        return std::nullopt;
    const bool next_day = hour == 24;
    if (next_day)
        hour = 0;

    const auto ref_s = std::chrono::duration_cast<std::chrono::seconds>(ref.time_since_epoch()).count();
    const int64_t ref_days = ref_s >= 0 ? ref_s / 86400 : (ref_s - 86399) / 86400;
    const detail::Civil c = detail::civilFromDays(ref_days);

    std::optional<TimePoint> best;
    for (int k = -1; k <= 1; ++k) {
        std::optional<TimePoint> t;
        if (month)
            t = detail::makeUtc(c.y + k, *month, *day, hour, minute);
        else if (day)
            t = detail::makeUtc(c.y, static_cast<int64_t>(c.m) + k, *day, hour, minute);
        else {
            const detail::Civil dc = detail::civilFromDays(ref_days + k);
            t = detail::makeUtc(dc.y, dc.m, dc.d, hour, minute);
        }
        if (!t)
            continue;
        if (next_day)
            *t += std::chrono::hours(24);
        // Candidates come in increasing order, so a strict < keeps the earlier one on a tie.
        if (!best || (*t > ref ? *t - ref : ref - *t) < (*best > ref ? *best - ref : ref - *best))
            best = t;
    }
    return best;
}

// The APDU header's time (hh:mm, plus mm/dd for T-opt 2/3) resolved against ref.
inline std::optional<TimePoint> apduTime(const ApduHeader& h, TimePoint ref)
{
    std::optional<unsigned> mo, dy;
    if (h.month && h.day) {
        mo = *h.month;
        dy = *h.day;
    }
    return resolveUtc(ref, mo, dy, h.hours, h.minutes);
}

}  // namespace fisb
}  // namespace skycued
