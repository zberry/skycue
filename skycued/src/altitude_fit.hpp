#pragma once

// Rolling least-squares fit of the GNSS-minus-baro altitude difference against
// barometric altitude over all aircraft. Pure: times are arguments, nothing
// here reads a clock. A track without a reported geometric altitude can use
// predict(baro) as an estimate.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>

namespace skycued {

struct AltitudeFitConfig {
    std::chrono::seconds window{120};
    std::size_t min_aircraft = 5, min_samples = 20;
    std::chrono::seconds bucket{5};  // at most one sample per aircraft per bucket
    int32_t min_span_ft = 5000;      // baro span needed for a slope, else the median
    double slope_min = -0.02, slope_max = 0.10;  // outside: the median
};

struct FitResult {
    double a = 0, b = 0;  // diff ~= a + b*baro (b = 0 for the median)
    bool linear = false;  // false: median fallback
    std::size_t samples = 0, aircraft = 0;  // after outlier rejection
    double p90_ft = 0;  // 90th percentile of |residual| over the kept points
    std::chrono::system_clock::time_point newest{};  // newest sample used
    double predict(double baro_ft) const { return a + b * baro_ft; }
};

class AltitudeFit {
public:
    using Clock = std::chrono::system_clock;
    explicit AltitudeFit(AltitudeFitConfig cfg = {});
    void add(uint32_t aircraft_key, Clock::time_point t, int32_t baro_ft, int32_t diff_ft);
    // Drops samples older than window, reduces to one point per aircraft (median baro, median diff),
    // fits, rejects points with |res| > max(150, 3*1.4826*median|res|), refits once. nullopt: no estimate.
    std::optional<FitResult> fit(Clock::time_point now);  // cached for 5 s of `now`
    std::size_t sampleCount() const;

private:
    struct Sample {
        Clock::time_point t;
        int32_t baro_ft, diff_ft;
    };
    static constexpr std::size_t kMaxPerAircraft = 64;
    AltitudeFitConfig cfg_;
    std::map<uint32_t, std::deque<Sample>> samples_;
    bool cache_valid_ = false;
    Clock::time_point cache_at_{};
    std::optional<FitResult> cache_;
};

}  // namespace skycued
