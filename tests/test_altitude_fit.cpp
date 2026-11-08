// AltitudeFit: rolling GNSS-minus-baro fit over a synthetic air mass.
// Aircraft i flies at baro = 3000 + 2500*i, diff = 100 + 0.027*baro + noise.
#define BOOST_TEST_MODULE altitude_fit
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

#include "altitude_fit.hpp"

using skycued::AltitudeFit;
using skycued::AltitudeFitConfig;
using Clock = AltitudeFit::Clock;
using std::chrono::seconds;

namespace {

const Clock::time_point kT0{seconds(1'700'000'000)};
constexpr double kA = 100.0, kB = 0.027;

// Fills `fit` with `aircraft` x `per` samples, 6 s apart starting at `t0`.
// Returns the injected noise of every sample. `outliers` get +3000 ft.
std::vector<int> fill(AltitudeFit& fit, int aircraft, int per, Clock::time_point t0 = kT0,
                      const std::set<int>& outliers = {}, double slope = kB, unsigned seed = 7) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> noise(-40, 40);
    std::vector<int> injected;
    for (int i = 0; i < aircraft; ++i) {
        for (int s = 0; s < per; ++s) {
            const int baro = 3000 + 2500 * i + 10 * s;
            const int n = noise(rng);
            injected.push_back(n);
            int diff = static_cast<int>(std::lround(kA + slope * baro)) + n;
            if (outliers.count(i)) diff += 3000;
            fit.add(static_cast<uint32_t>(i + 1), t0 + seconds(6 * s), baro, diff);
        }
    }
    return injected;
}

}  // namespace

BOOST_AUTO_TEST_CASE(recovers_known_a_b) {
    AltitudeFit fit;
    fill(fit, 14, 6);
    const auto r = fit.fit(kT0 + seconds(40));
    BOOST_REQUIRE(r);
    BOOST_TEST(r->linear);
    BOOST_TEST(std::fabs(r->a - kA) <= 40.0);
    BOOST_TEST(std::fabs(r->b - kB) <= 0.002);
    BOOST_TEST(r->aircraft == 14u);
    BOOST_TEST(r->samples == 84u);
    BOOST_TEST_MESSAGE("recovered a=" << r->a << " b=" << r->b << " p90=" << r->p90_ft);
}

BOOST_AUTO_TEST_CASE(p90_matches_noise) {
    AltitudeFit fit;
    // One sample per aircraft, so the point residual is the injected noise.
    auto noise = fill(fit, 60, 1);
    std::vector<double> mag;
    for (int n : noise) mag.push_back(std::abs(n));
    std::sort(mag.begin(), mag.end());
    const double expect = mag[static_cast<std::size_t>(std::ceil(0.9 * static_cast<double>(mag.size()))) - 1];
    const auto r = fit.fit(kT0 + seconds(10));
    BOOST_REQUIRE(r);
    BOOST_TEST(std::fabs(r->p90_ft - expect) <= 25.0);
}

BOOST_AUTO_TEST_CASE(one_sample_per_bucket) {
    AltitudeFit fit;
    for (int i = 0; i < 50; ++i) {
        fit.add(1, kT0 + std::chrono::milliseconds(80 * i), 30000, 900);
    }
    BOOST_TEST(fit.sampleCount() == 1u);
    fit.add(1, kT0 + seconds(5), 30000, 900);
    BOOST_TEST(fit.sampleCount() == 2u);
}

BOOST_AUTO_TEST_CASE(needs_5_aircraft_and_20_samples) {
    {
        AltitudeFit fit;
        fill(fit, 4, 10);
        const auto r = fit.fit(kT0 + seconds(60));
        BOOST_REQUIRE(r);
        BOOST_TEST(!r->linear);
        BOOST_TEST(r->b == 0.0);
    }
    {  // the median fallback needs 3 distinct aircraft
        AltitudeFit two;
        fill(two, 2, 10);
        BOOST_TEST(!two.fit(kT0 + seconds(60)).has_value());
        AltitudeFit three;
        fill(three, 3, 10);
        const auto r = three.fit(kT0 + seconds(60));
        BOOST_REQUIRE(r);
        BOOST_TEST(!r->linear);
    }
    {
        AltitudeFit fit;
        fill(fit, 5, 4);
        const auto r = fit.fit(kT0 + seconds(60));
        BOOST_REQUIRE(r);
        BOOST_TEST(r->linear);
    }
    {
        AltitudeFit fit;
        fill(fit, 5, 3);
        const auto r = fit.fit(kT0 + seconds(60));
        BOOST_REQUIRE(r);
        BOOST_TEST(!r->linear);
    }
    AltitudeFit empty;
    BOOST_TEST(!empty.fit(kT0).has_value());
}

BOOST_AUTO_TEST_CASE(narrow_span_uses_median) {
    AltitudeFit fit;
    for (int i = 0; i < 8; ++i) {
        for (int s = 0; s < 4; ++s) {
            const int baro = 20000 + 350 * i;  // 2450 ft wide
            fit.add(static_cast<uint32_t>(i + 1), kT0 + seconds(6 * s), baro,
                    static_cast<int>(kA + kB * baro) + (i % 3 - 1) * 10);
        }
    }
    const auto r = fit.fit(kT0 + seconds(30));
    BOOST_REQUIRE(r);
    BOOST_TEST(!r->linear);
    BOOST_TEST(r->b == 0.0);
    BOOST_TEST(std::fabs(r->a - (kA + kB * 21000)) <= 60.0);
}

BOOST_AUTO_TEST_CASE(slope_out_of_range_uses_median) {
    AltitudeFit fit;
    fill(fit, 12, 4, kT0, {}, 0.3);
    const auto r = fit.fit(kT0 + seconds(30));
    BOOST_REQUIRE(r);
    BOOST_TEST(!r->linear);
    BOOST_TEST(r->b == 0.0);
}

BOOST_AUTO_TEST_CASE(window_ages_out) {
    {
        AltitudeFit fit;
        fill(fit, 8, 4);
        BOOST_TEST(fit.fit(kT0 + seconds(30)).has_value());
        BOOST_TEST(!fit.fit(kT0 + seconds(30 + 121 + 18)).has_value());
        BOOST_TEST(fit.sampleCount() == 0u);
    }
    {
        // Two cohorts, 100 s apart, different offsets; at t0+130 only the later counts.
        AltitudeFit fit;
        for (int i = 0; i < 8; ++i) {
            const int baro = 3000 + 4000 * i;
            fit.add(static_cast<uint32_t>(i + 1), kT0, baro, 100 + baro / 40);
            fit.add(static_cast<uint32_t>(i + 1), kT0 + seconds(100), baro, 100 + baro / 40 + 200);
        }
        const auto r = fit.fit(kT0 + seconds(130));
        BOOST_REQUIRE(r);
        BOOST_TEST(r->samples == 8u);
        BOOST_TEST((r->newest == kT0 + seconds(100)));
        BOOST_TEST(std::fabs(r->a - 725.0) <= 60.0);  // median of the later cohort only
    }
    AltitudeFit fit;
    fit.add(1, kT0, 30000, 900);
    BOOST_TEST(!fit.fit(kT0 + seconds(121)).has_value());
}

BOOST_AUTO_TEST_CASE(outlier_aircraft_rejected) {
    AltitudeFit fit;
    fill(fit, 12, 6, kT0, {3});
    const auto r = fit.fit(kT0 + seconds(40));
    BOOST_REQUIRE(r);
    BOOST_TEST(r->linear);
    BOOST_TEST(r->aircraft == 11u);
    BOOST_TEST(std::fabs(r->a - kA) <= 40.0);  // the noise bound; 10 % of 100 ft is below the noise
    BOOST_TEST(std::fabs(r->b - kB) <= 0.1 * kB);
}

BOOST_AUTO_TEST_CASE(two_outliers_of_twelve) {
    AltitudeFit fit;
    fill(fit, 12, 6, kT0, {3, 8});
    const auto r = fit.fit(kT0 + seconds(40));
    BOOST_REQUIRE(r);
    BOOST_TEST(r->linear);
    BOOST_TEST(r->aircraft == 10u);
    BOOST_TEST(std::fabs(r->a - kA) <= 40.0);  // the noise bound; 10 % of 100 ft is below the noise
    BOOST_TEST(std::fabs(r->b - kB) <= 0.1 * kB);
}

BOOST_AUTO_TEST_CASE(sample_clock_backwards_is_stored) {
    AltitudeFit fit;
    fit.add(1, kT0 + seconds(100), 30000, 900);
    fit.add(1, kT0 + seconds(50), 30000, 900);  // older than the newest stored: kept, in order
    BOOST_TEST(fit.sampleCount() == 2u);
    fit.add(1, kT0 + seconds(52), 30000, 900);  // inside the bucket of a neighbour: dropped
    BOOST_TEST(fit.sampleCount() == 2u);
    fit.add(1, kT0 + seconds(60), 30000, 900);
    BOOST_TEST(fit.sampleCount() == 3u);
    fit.add(2, kT0 + seconds(60), 31000, 900);  // two more aircraft: the median needs 3
    fit.add(3, kT0 + seconds(60), 32000, 900);
    const auto r = fit.fit(kT0 + seconds(70));  // `now` earlier than the newest sample
    BOOST_REQUIRE(r);
    BOOST_TEST(r->a == 900.0);
    BOOST_TEST(r->samples == 5u);
    BOOST_TEST((r->newest == kT0 + seconds(100)));
}

BOOST_AUTO_TEST_CASE(samples_from_the_future_pruned) {
    AltitudeFit fit;
    fit.add(1, kT0 + seconds(1000), 30000, 5000);  // a forward jump, then the clock stepped back
    fit.add(2, kT0 + seconds(10), 30000, 900);
    fit.add(3, kT0 + seconds(10), 31000, 900);
    fit.add(4, kT0 + seconds(10), 32000, 900);
    fit.add(5, kT0 + seconds(10), 33000, 900);
    const auto r = fit.fit(kT0 + seconds(20));
    BOOST_REQUIRE(r);
    BOOST_TEST(r->aircraft == 4u);
    BOOST_TEST(r->a == 900.0);
    BOOST_TEST(fit.sampleCount() == 4u);
}

BOOST_AUTO_TEST_CASE(fit_cached_5s) {
    AltitudeFit fit;
    fill(fit, 8, 4);
    const auto first = fit.fit(kT0 + seconds(30));
    BOOST_REQUIRE(first);
    // An in-band sample for an existing aircraft changes `newest` and `samples`.
    fit.add(1, kT0 + seconds(31), 3000 + 40, static_cast<int>(kA + kB * 3040));
    const auto cached = fit.fit(kT0 + seconds(34));
    BOOST_REQUIRE(cached);
    BOOST_TEST(cached->samples == first->samples);
    BOOST_TEST((cached->newest == first->newest));
    const auto later = fit.fit(kT0 + seconds(36));
    BOOST_REQUIRE(later);
    BOOST_TEST(later->samples == first->samples + 1u);
    BOOST_TEST((later->newest == kT0 + seconds(31)));
}

BOOST_AUTO_TEST_CASE(tisb_and_icao_same_address_separate) {
    // The engine keys the fit by address type and address; the same 24 bits from an ICAO
    // transponder and from a TIS-B copy are two aircraft here.
    auto key = [](uint64_t type, uint32_t addr) { return (type << 32) | addr; };
    AltitudeFit fit;
    fit.add(key(0, 0xABC123), kT0, 30000, 900);
    fit.add(key(2, 0xABC123), kT0, 20000, 900);
    fit.add(key(0, 0x111111), kT0, 25000, 900);
    const auto r = fit.fit(kT0 + seconds(10));
    BOOST_REQUIRE(r);
    BOOST_TEST(r->aircraft == 3u);
}
