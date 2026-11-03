#pragma once

// Upper-bound waits in the tests scale with SKYCUE_TEST_SLOW, which the
// sanitizer and fuzz builds set (4 by default, SKYCUE_TEST_SLOW_FACTOR).
// Only waits for something that must happen are scaled; a check that
// something did not happen within a time is left alone.

#include <chrono>
#include <cstdlib>

// The factor in SKYCUE_TEST_SLOW, read on every call: a whole decimal
// number of at least 1, clamped to 20. Anything else (unset, empty, 0,
// negative, trailing characters) is 1.
inline int testSlowFactor()
{
    constexpr int kMax = 20;
    const char* s = std::getenv("SKYCUE_TEST_SLOW");
    if (s == nullptr || *s == '\0')
        return 1;
    int v = 0;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9')
            return 1;
        if (v <= kMax)
            v = v * 10 + (*p - '0');
    }
    if (v < 1)
        return 1;
    return v > kMax ? kMax : v;
}

template <class Rep, class Period>
inline std::chrono::milliseconds slow(std::chrono::duration<Rep, Period> d)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(d) * testSlowFactor();
}
