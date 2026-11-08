// Per-track geometric altitude policy: reported or estimated.
#define BOOST_TEST_MODULE geom_altitude
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>

#include "geom_altitude.hpp"
#include "track_table.hpp"

using namespace skycued;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

const Clock::time_point kT0{seconds(1'700'000'000)};

TrackObservation obsAt(Clock::time_point t)
{
    TrackObservation o;
    o.id = skycue::TrackId{0xABC123, skycue::AddressType::icao};
    o.received_time = t;
    return o;
}

// A track as the table would hold it: merged, with the learned history.
struct Fixture {
    TrackTable table;
    skycue::TrackId id{0xABC123, skycue::AddressType::icao};
    std::optional<int32_t> feed(const TrackObservation& o)
    {
        TrackState& s = table.apply(o);
        return learnGeomDiff(s.geom, o);
    }
    TrackState& st() { return *table.find(id); }
    GeomResult at(Clock::time_point now, const std::optional<FitResult>& fit = std::nullopt)
    {
        return resolveGeomAltitude(st(), now, fit);
    }
};

FitResult makeFit(double a, double b, double p90 = 80, bool linear = true)
{
    FitResult f;
    f.a = a;
    f.b = b;
    f.linear = linear;
    f.samples = 40;
    f.aircraft = 8;
    f.p90_ft = p90;
    f.newest = kT0 + seconds(100);
    return f;
}

// A baro-only observation.
TrackObservation baroObs(Clock::time_point t, int32_t baro)
{
    TrackObservation o = obsAt(t);
    o.alt_baro_ft = baro;
    return o;
}

} // namespace

BOOST_AUTO_TEST_CASE(learn_from_tc19_diff)
{
    Fixture f;
    TrackObservation o = obsAt(kT0);
    o.alt_baro_ft = 30000;
    o.geom_minus_baro_ft = 500;
    const auto sample = f.feed(o);
    BOOST_REQUIRE(sample);
    BOOST_CHECK_EQUAL(*sample, 500);
    BOOST_CHECK_EQUAL(*f.st().geom.diff_ft, 500);
    BOOST_CHECK_EQUAL(*f.st().geom.baro_at_diff_ft, 30000);
    BOOST_CHECK(f.st().geom.diff_time == kT0);
}

BOOST_AUTO_TEST_CASE(learn_from_tc2022_with_baro_within_2s)
{
    Fixture f;
    TrackObservation o = obsAt(kT0);
    o.alt_geom_ft = 31000;
    o.alt_baro_ft = 30525;
    o.alt_baro_time = kT0 - milliseconds(1500);
    const auto sample = f.feed(o);
    BOOST_REQUIRE(sample);
    BOOST_CHECK_EQUAL(*sample, 475);
    BOOST_CHECK_EQUAL(*f.st().geom.baro_at_diff_ft, 30525);
}

BOOST_AUTO_TEST_CASE(no_learning_with_baro_3s_apart)
{
    Fixture f;
    TrackObservation o = obsAt(kT0);
    o.alt_geom_ft = 31000;
    o.alt_baro_ft = 30525;
    o.alt_baro_time = kT0 - seconds(3);
    BOOST_CHECK(!f.feed(o));
    BOOST_CHECK(!f.st().geom.diff_ft);
}

BOOST_AUTO_TEST_CASE(diff_without_paired_baro_is_learned_but_not_a_sample)
{
    Fixture f;
    TrackObservation o = obsAt(kT0);
    o.geom_minus_baro_ft = 500;  // no baro at all
    BOOST_CHECK(!f.feed(o));
    BOOST_CHECK_EQUAL(*f.st().geom.diff_ft, 500);
    BOOST_CHECK(!f.st().geom.baro_at_diff_ft);
    TrackObservation p = obsAt(kT0 + seconds(10));
    p.geom_minus_baro_ft = 510;
    p.alt_baro_ft = 30000;
    p.alt_baro_time = kT0 + seconds(5);  // 5 s apart
    BOOST_CHECK(!f.feed(p));
    BOOST_CHECK_EQUAL(*f.st().geom.diff_ft, 510);
    BOOST_CHECK(!f.st().geom.baro_at_diff_ft);
}

BOOST_AUTO_TEST_CASE(reported_while_no_newer_baro)
{
    Fixture f;
    TrackObservation o = obsAt(kT0);
    o.alt_geom_ft = 31000;
    f.feed(o);
    // No baro at all, no fit: still reported, up to and including 60 s.
    auto r = f.at(kT0 + seconds(45));
    BOOST_CHECK(r.source == GeomSource::reported);
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, 31000);
    BOOST_CHECK(!r.uncertainty_ft);
    BOOST_REQUIRE(r.age_s);
    BOOST_CHECK_CLOSE(*r.age_s, 45.0, 1e-9);
    BOOST_CHECK(f.at(kT0 + seconds(60)).source == GeomSource::reported);
    BOOST_CHECK(f.at(kT0 + milliseconds(60001)).source == GeomSource::none);
    // A baro 2 s newer than the report does not displace it.
    f.feed(baroObs(kT0 + seconds(2), 30500));
    BOOST_CHECK(f.at(kT0 + seconds(2)).source == GeomSource::reported);
}

BOOST_AUTO_TEST_CASE(newer_baro_switches_to_own_diff_estimate)
{
    Fixture f;
    TrackObservation o = obsAt(kT0);
    o.alt_baro_ft = 30000;
    o.alt_geom_ft = 30480;
    f.feed(o);  // learns 480
    f.feed(baroObs(kT0 + seconds(5), 30050));
    const auto r = f.at(kT0 + seconds(5));
    BOOST_CHECK(r.source == GeomSource::estimated);
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, 30050 + 480 + 1);  // no fit: b = 0.027, round(0.027*50) = 1
    BOOST_CHECK_EQUAL(*r.uncertainty_ft, 75);
}

BOOST_AUTO_TEST_CASE(own_diff_after_60s)
{
    Fixture f;
    TrackObservation o = obsAt(kT0);
    o.alt_baro_ft = 30000;
    o.geom_minus_baro_ft = 500;
    o.alt_geom_ft = 30500;
    f.feed(o);
    f.feed(baroObs(kT0 + seconds(70), 34000));

    // With a fit: b = 0.01.
    const auto fit = makeFit(100, 0.01);
    auto r = f.at(kT0 + seconds(70), fit);
    BOOST_CHECK(r.source == GeomSource::estimated);
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, 34000 + 500 + 40);
    BOOST_CHECK_EQUAL(*r.uncertainty_ft, 125 + 8);  // 10 s past 60: 125 + 200*10/240 = 133
    BOOST_CHECK_CLOSE(*r.age_s, 70.0, 1e-9);
    BOOST_CHECK_EQUAL(*r.geom_minus_baro_ft, 500);

    // Without one: b = 0.027.
    r = f.at(kT0 + seconds(70));
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, 34000 + 500 + 108);

    // The median fallback has b = 0 and the own difference then takes no slope term.
    r = f.at(kT0 + seconds(70), makeFit(300, 0, 80, false));
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, 34500);
}

BOOST_AUTO_TEST_CASE(own_diff_uncertainty_ramps_125_to_325)
{
    Fixture f;
    TrackObservation o = obsAt(kT0);
    o.alt_baro_ft = 30000;
    o.geom_minus_baro_ft = 500;
    f.feed(o);
    for (const auto& [age, want] : {std::pair<int, int>{61, 126}, {180, 225}, {300, 325}}) {
        f.feed(baroObs(kT0 + seconds(age), 30000));
        BOOST_CHECK_EQUAL(*f.at(kT0 + seconds(age)).uncertainty_ft, want);
    }
}

BOOST_AUTO_TEST_CASE(own_diff_expires_at_300s_then_fit)
{
    Fixture f;
    TrackObservation o = obsAt(kT0);
    o.alt_baro_ft = 30000;
    o.geom_minus_baro_ft = 500;
    f.feed(o);
    f.feed(baroObs(kT0 + seconds(300), 30000));
    const auto fit = makeFit(100, 0.027);
    BOOST_CHECK_EQUAL(*f.at(kT0 + seconds(300), fit).alt_geom_ft, 30500);
    f.feed(baroObs(kT0 + seconds(301), 30000));
    const auto r = f.at(kT0 + seconds(301), fit);
    BOOST_CHECK(r.source == GeomSource::estimated);
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, std::lround(30000 + 100 + 0.027 * 30000));
    // The last reported difference is still shown.
    BOOST_CHECK_EQUAL(*r.geom_minus_baro_ft, 500);
}

BOOST_AUTO_TEST_CASE(fit_estimate_formula)
{
    Fixture f;
    f.feed(baroObs(kT0, 20000));
    auto r = f.at(kT0, makeFit(100, 0.027, 130));
    BOOST_CHECK(r.source == GeomSource::estimated);
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, std::lround(20000 + 100 + 0.027 * 20000));
    BOOST_CHECK_EQUAL(*r.uncertainty_ft, 150);  // ceil(130/25)*25
    BOOST_CHECK_CLOSE(*r.age_s, -100.0, 1e-9);  // fit.newest is 100 s after kT0 in this fixture
    BOOST_CHECK(!r.geom_minus_baro_ft);
    // Floor of 100 ft.
    BOOST_CHECK_EQUAL(*f.at(kT0, makeFit(100, 0.027, 30)).uncertainty_ft, 100);
    BOOST_CHECK_EQUAL(*f.at(kT0, makeFit(100, 0.027, 100)).uncertainty_ft, 100);
    BOOST_CHECK_EQUAL(*f.at(kT0, makeFit(100, 0.027, 100.5)).uncertainty_ft, 125);
}

BOOST_AUTO_TEST_CASE(fit_with_median_fallback)
{
    Fixture f;
    f.feed(baroObs(kT0, 20000));
    const auto r = f.at(kT0, makeFit(420, 0, 90, false));
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, 20420);
    BOOST_CHECK_EQUAL(*r.uncertainty_ft, 100);
}

BOOST_AUTO_TEST_CASE(no_fit_no_estimate)
{
    Fixture f;
    f.feed(baroObs(kT0, 20000));
    const auto r = f.at(kT0);
    BOOST_CHECK(r.source == GeomSource::none);
    BOOST_CHECK(!r.alt_geom_ft);
    BOOST_CHECK(!r.uncertainty_ft);
    BOOST_CHECK(!r.age_s);
}

BOOST_AUTO_TEST_CASE(stale_baro_gives_no_estimate)
{
    Fixture f;
    f.feed(baroObs(kT0, 20000));
    const auto fit = makeFit(100, 0.027);
    BOOST_CHECK(f.at(kT0 + seconds(10), fit).source == GeomSource::estimated);
    BOOST_CHECK(f.at(kT0 + seconds(11), fit).source == GeomSource::none);
    // A carried baro keeps its measurement time, not the receive time.
    TrackObservation o = obsAt(kT0 + seconds(30));
    o.alt_baro_ft = 20100;
    o.alt_baro_time = kT0 + seconds(18);
    f.feed(o);
    BOOST_CHECK(f.at(kT0 + seconds(30), fit).source == GeomSource::none);
    BOOST_CHECK(f.at(kT0 + seconds(28), fit).source == GeomSource::estimated);
}

BOOST_AUTO_TEST_CASE(ground_gives_none)
{
    Fixture f;
    TrackObservation o = baroObs(kT0, 0);
    o.on_ground = true;
    f.feed(o);
    BOOST_CHECK(f.at(kT0, makeFit(100, 0.027)).source == GeomSource::none);
    // Airborne again.
    TrackObservation air = baroObs(kT0 + seconds(1), 1500);
    air.on_ground = false;
    f.feed(air);
    BOOST_CHECK(f.at(kT0 + seconds(1), makeFit(100, 0.027)).source == GeomSource::estimated);
}

BOOST_AUTO_TEST_CASE(no_baro_gives_none)
{
    Fixture f;
    TrackObservation o = obsAt(kT0);
    o.lat = 38.9;
    o.lon = -77.0;
    o.position_time = kT0;
    f.feed(o);
    const auto r = f.at(kT0, makeFit(100, 0.027));
    BOOST_CHECK(r.source == GeomSource::none);
    BOOST_CHECK(!r.alt_geom_ft);
}

BOOST_AUTO_TEST_CASE(no_jump_at_report_expiry)
{
    // Baro constant and refreshed each second across 60 s: the value never moves by more
    // than 25 ft and the source flips exactly once (reported -> estimated).
    Fixture f;
    TrackObservation o = obsAt(kT0);
    o.alt_baro_ft = 30000;
    o.alt_geom_ft = 30480;
    f.feed(o);  // learns diff 480
    const auto fit = makeFit(100, 0.027);
    std::optional<int32_t> prev;
    int flips = 0;
    GeomSource src = GeomSource::reported;
    for (int ms = 0; ms <= 61000; ms += 100) {
        const auto t = kT0 + milliseconds(ms);
        if (ms % 1000 == 0 && ms > 0)
            f.feed(baroObs(t, 30000));
        const auto r = f.at(t, fit);
        BOOST_REQUIRE(r.alt_geom_ft);
        if (prev)
            BOOST_CHECK_LE(std::abs(*r.alt_geom_ft - *prev), 25);
        prev = r.alt_geom_ft;
        if (r.source != src)
            ++flips;
        src = r.source;
    }
    BOOST_CHECK_EQUAL(flips, 1);
}

BOOST_AUTO_TEST_CASE(alternating_source_is_continuous)
{
    // A report every 90 s for 10 minutes, baro ramping 10 ft/s, sampled every second:
    // every step, flips included, stays within the baro change plus 25 ft.
    Fixture f;
    const auto fit = makeFit(100, 0.027);
    std::optional<int32_t> prev_geom;
    for (int t = 0; t <= 600; ++t) {
        const int32_t baro = 20000 + 10 * t;
        TrackObservation o = baroObs(kT0 + seconds(t), baro);
        if (t % 90 == 0)
            o.alt_geom_ft = baro + 480;
        f.feed(o);
        const auto r = f.at(kT0 + seconds(t), fit);
        BOOST_REQUIRE(r.alt_geom_ft);
        if (prev_geom)
            BOOST_CHECK_LE(std::abs(*r.alt_geom_ft - *prev_geom), 10 + 25);
        prev_geom = r.alt_geom_ft;
    }
}

BOOST_AUTO_TEST_CASE(source_flips_once_per_report)
{
    Fixture f;
    const auto fit = makeFit(100, 0.027);
    int flips = 0, reports = 0;
    GeomSource prev = GeomSource::none;
    for (int t = 0; t <= 600; ++t) {
        TrackObservation o = baroObs(kT0 + seconds(t), 25000);
        if (t % 90 == 0) {
            o.alt_geom_ft = 25480;
            ++reports;
        }
        f.feed(o);
        const auto r = f.at(kT0 + seconds(t), fit);
        if (r.source != prev)
            ++flips;
        prev = r.source;
    }
    // none->reported once; each report then flips to estimated (first newer baro) and the next report back.
    BOOST_CHECK_EQUAL(flips, 2 * reports);
}

BOOST_AUTO_TEST_CASE(tisb_target_gets_fit_estimate)
{
    Fixture f;
    TrackObservation o = baroObs(kT0, 12000);
    o.source = skycue::Source::tisb;
    f.feed(o);
    const auto r = f.at(kT0, makeFit(100, 0.027));
    BOOST_CHECK(r.source == GeomSource::estimated);
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, std::lround(12000 + 100 + 0.027 * 12000));
    BOOST_CHECK(!r.geom_minus_baro_ft);
}
