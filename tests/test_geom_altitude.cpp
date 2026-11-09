// Per-track geometric altitude policy: reported or estimated.
#define BOOST_TEST_MODULE geom_altitude
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "altitude_fit.hpp"
#include "decode/uat_decoder.hpp"
#include "geom_altitude.hpp"
#include "readsb_json_source.hpp"
#include "track_table.hpp"
#include "uat_test_support.hpp"

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
struct Harness {
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
    Harness f;
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
    Harness f;
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
    Harness f;
    TrackObservation o = obsAt(kT0);
    o.alt_geom_ft = 31000;
    o.alt_baro_ft = 30525;
    o.alt_baro_time = kT0 - seconds(3);
    BOOST_CHECK(!f.feed(o));
    BOOST_CHECK(!f.st().geom.diff_ft);
}

BOOST_AUTO_TEST_CASE(diff_without_paired_baro_is_learned_but_not_a_sample)
{
    Harness f;
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
    Harness f;
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
    Harness f;
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
    Harness f;
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

    // Under the median fallback (b = 0) the own difference still takes the default slope.
    r = f.at(kT0 + seconds(70), makeFit(300, 0, 80, false));
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, 34000 + 500 + 108);
}

BOOST_AUTO_TEST_CASE(own_diff_uncertainty_ramps_125_to_325)
{
    Harness f;
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
    Harness f;
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
    Harness f;
    f.feed(baroObs(kT0, 20000));
    auto r = f.at(kT0, makeFit(100, 0.027, 130));
    BOOST_CHECK(r.source == GeomSource::estimated);
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, std::lround(20000 + 100 + 0.027 * 20000));
    BOOST_CHECK_EQUAL(*r.uncertainty_ft, 150);  // ceil(130/25)*25
    BOOST_CHECK_CLOSE(*r.age_s, -100.0, 1e-9);  // fit.newest is 100 s after kT0 in this sample
    BOOST_CHECK(!r.geom_minus_baro_ft);
    // Floor of 100 ft.
    BOOST_CHECK_EQUAL(*f.at(kT0, makeFit(100, 0.027, 30)).uncertainty_ft, 100);
    BOOST_CHECK_EQUAL(*f.at(kT0, makeFit(100, 0.027, 100)).uncertainty_ft, 100);
    BOOST_CHECK_EQUAL(*f.at(kT0, makeFit(100, 0.027, 100.5)).uncertainty_ft, 125);
}

BOOST_AUTO_TEST_CASE(fit_with_median_fallback)
{
    Harness f;
    f.feed(baroObs(kT0, 20000));
    const auto r = f.at(kT0, makeFit(420, 0, 90, false));
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, 20420);
    BOOST_CHECK_EQUAL(*r.uncertainty_ft, 600);
}

BOOST_AUTO_TEST_CASE(median_fallback_uncertainty_floor_600)
{
    Harness f;
    f.feed(baroObs(kT0, 20000));
    BOOST_CHECK_EQUAL(*f.at(kT0, makeFit(420, 0, 30, false)).uncertainty_ft, 600);
    BOOST_CHECK_EQUAL(*f.at(kT0, makeFit(420, 0, 600, false)).uncertainty_ft, 600);
    BOOST_CHECK_EQUAL(*f.at(kT0, makeFit(420, 0, 610, false)).uncertainty_ft, 625);  // p90 rounded up to 25
}

BOOST_AUTO_TEST_CASE(reported_kept_when_nothing_better)
{
    // A newer baro, no difference of its own, no fit: a reported value <= 60 s old stays.
    Harness f;
    TrackObservation g = obsAt(kT0);
    g.alt_geom_ft = 30500;
    f.feed(g);
    f.feed(baroObs(kT0 + seconds(5), 30200));
    auto r = f.at(kT0 + seconds(6));
    BOOST_CHECK(r.source == GeomSource::reported);
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, 30500);
    // Older than 60 s: gone.
    f.feed(baroObs(kT0 + seconds(65), 30200));
    BOOST_CHECK(f.at(kT0 + seconds(66)).source == GeomSource::none);
}

BOOST_AUTO_TEST_CASE(carried_diff_is_one_fit_sample)
{
    // 12 observations carrying one difference (its own time, a baro paired with it at first):
    // only the first yields a fit sample; the repeats are not new samples (same time, no new pair).
    Harness f;
    int samples = 0;
    for (int i = 0; i < 12; ++i) {
        TrackObservation o = obsAt(kT0 + seconds(20 + i));
        o.alt_baro_ft = 30000 + 10 * i;
        o.alt_baro_time = kT0 + seconds(i == 0 ? 0 : 20 + i);
        o.geom_minus_baro_ft = 500;
        o.geom_minus_baro_time = kT0;
        if (f.feed(o))
            ++samples;
    }
    BOOST_CHECK_EQUAL(samples, 1);  // the first pairs with its baro; the carried repeats, 20 s on, do not
    TrackObservation first = obsAt(kT0);
    first.alt_baro_ft = 30000;
    first.geom_minus_baro_ft = 500;
    Harness g;
    BOOST_CHECK(g.feed(first).has_value());
    for (int i = 1; i < 12; ++i) {
        TrackObservation o = obsAt(kT0 + milliseconds(100 * i));
        o.alt_baro_ft = 30000 + 10 * i;
        o.geom_minus_baro_ft = 500;
        o.geom_minus_baro_time = kT0;
        // The same difference time returns with a baro paired within 2 s: the history keeps
        // one difference; as a fit sample the engine's 5 s bucket takes only the first.
        g.feed(o);
    }
    AltitudeFit fit;
    for (int i = 0; i < 12; ++i)
        fit.add(1, g.st().geom.diff_time, 30000 + 10 * i, 500);
    BOOST_CHECK_EQUAL(fit.sampleCount(), 1u);
}

BOOST_AUTO_TEST_CASE(same_diff_unpaired_keeps_earlier_baro_at_diff)
{
    Harness f;
    TrackObservation first = obsAt(kT0);
    first.alt_baro_ft = 30000;
    first.geom_minus_baro_ft = 500;
    f.feed(first);
    TrackObservation again = obsAt(kT0 + seconds(30));
    again.alt_baro_ft = 31000;
    again.alt_baro_time = kT0 + seconds(30);
    again.geom_minus_baro_ft = 500;
    again.geom_minus_baro_time = kT0;
    BOOST_CHECK(!f.feed(again).has_value());
    BOOST_REQUIRE(f.st().geom.baro_at_diff_ft);
    BOOST_CHECK_EQUAL(*f.st().geom.baro_at_diff_ft, 30000);
}

BOOST_AUTO_TEST_CASE(no_fit_no_estimate)
{
    Harness f;
    f.feed(baroObs(kT0, 20000));
    const auto r = f.at(kT0);
    BOOST_CHECK(r.source == GeomSource::none);
    BOOST_CHECK(!r.alt_geom_ft);
    BOOST_CHECK(!r.uncertainty_ft);
    BOOST_CHECK(!r.age_s);
}

BOOST_AUTO_TEST_CASE(stale_baro_gives_no_estimate)
{
    Harness f;
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
    Harness f;
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
    Harness f;
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
    Harness f;
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
    Harness f;
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
    Harness f;
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
    Harness f;
    TrackObservation o = baroObs(kT0, 12000);
    o.source = skycue::Source::tisb;
    f.feed(o);
    const auto r = f.at(kT0, makeFit(100, 0.027));
    BOOST_CHECK(r.source == GeomSource::estimated);
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, std::lround(12000 + 100 + 0.027 * 12000));
    BOOST_CHECK(!r.geom_minus_baro_ft);
}

// ---- UAT-shaped observations through the engine's altitude path ----

namespace {

using skycued::testing::encodeUat;
using skycued::testing::UatSpec;

// What Engine::handleObservation does with altitudes, nothing else: merge into the
// table, learn the track's difference, feed the population fit, resolve on the data clock.
struct MiniEngine {
    TrackTable table;
    AltitudeFit fit;
    void feed(const TrackObservation& o)
    {
        TrackState& s = table.apply(o);
        if (const auto d = learnGeomDiff(s.geom, o); d && o.alt_baro_ft)
            fit.add((static_cast<uint64_t>(s.id.type) << 32) | s.id.address, s.geom.diff_time, *o.alt_baro_ft, *d);
    }
    TrackState& track(const skycue::TrackId& id) { return *table.find(id); }
    GeomResult resolve(const skycue::TrackId& id, Clock::time_point now)
    {
        return resolveGeomAltitude(track(id), now, fit.fit(now));
    }
};

// A UAT feed: frames in, the decoder's observations into a MiniEngine.
struct UatRig {
    MiniEngine eng;
    decode::UatDecoder dec;
    std::vector<TrackObservation> seen;
    static decode::DecoderConfig cfg()
    {
        decode::DecoderConfig c;
        c.reference = decode::cpr::Position{52.0, 4.5};
        return c;
    }
    UatRig() : dec(cfg(), "uat1")
    {
        dec.onObservation([this](const TrackObservation& o) {
            seen.push_back(o);
            eng.feed(o);
        });
    }
    void frame(const UatSpec& s, Clock::time_point at)
    {
        decode::uat::UatFrame f;
        f.bytes = encodeUat(s);
        dec.feed(f, at);
    }
};

UatSpec uatAirborne(uint32_t address, int baro_ft, std::optional<int> geom_ft)
{
    UatSpec s;
    s.type = 1;
    s.address = address;
    s.lat = 52.1 + 0.001 * (address & 0xFF);
    s.lon = 4.6;
    s.alt_ft = baro_ft;
    s.nic = 8;
    s.ns_kt = 100;
    s.ew_kt = 100;
    s.call = "TEST1";
    s.aux_alt_ft = geom_ft;
    return s;
}

// GNSS minus baro the synthetic population follows, rounded to the UAT 25 ft step.
int uatDiff(int baro) { return 25 * static_cast<int>(std::lround((150 + 0.02 * baro) / 25.0)); }

// Twelve UAT aircraft, 3000 ft apart from 3000 ft (33000 ft of span), three
// long frames each 6 s apart. Returns the time of the last frame.
Clock::time_point seedUatPopulation(UatRig& rig, Clock::time_point t0)
{
    Clock::time_point last = t0;
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 12; ++i) {
            last = t0 + seconds(6 * k);
            const int baro = 3000 + 3000 * i;
            rig.frame(uatAirborne(0xA00000 + static_cast<uint32_t>(i), baro, baro + uatDiff(baro)), last);
        }
    return last;
}

}  // namespace

BOOST_AUTO_TEST_CASE(uat_both_altitudes_yield_reported_difference)
{
    UatRig rig;
    const skycue::TrackId id{0xA66EF1, skycue::AddressType::icao};
    rig.frame(uatAirborne(0xA66EF1, 5000, 5175), kT0);
    BOOST_REQUIRE_EQUAL(rig.seen.size(), 1u);
    BOOST_REQUIRE(rig.seen[0].geom_minus_baro_ft);
    BOOST_CHECK_EQUAL(*rig.seen[0].geom_minus_baro_ft, 175);
    const auto r = rig.eng.resolve(id, kT0);
    BOOST_CHECK(r.source == GeomSource::reported);
    BOOST_REQUIRE(r.alt_geom_ft);
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, 5175);
    BOOST_CHECK(!r.uncertainty_ft);
    BOOST_REQUIRE(r.geom_minus_baro_ft);
    BOOST_CHECK_EQUAL(*r.geom_minus_baro_ft, 175);
    BOOST_REQUIRE(rig.seen[0].geom_minus_baro_time);
    BOOST_CHECK(*rig.seen[0].geom_minus_baro_time == kT0);
    BOOST_CHECK(rig.eng.track(id).geom.diff_time == kT0);
}

BOOST_AUTO_TEST_CASE(uat_difference_feeds_the_population_fit)
{
    UatRig rig;
    const auto last = seedUatPopulation(rig, kT0);
    const auto fit = rig.eng.fit.fit(last);
    BOOST_REQUIRE(fit);
    BOOST_CHECK(fit->linear);
    BOOST_CHECK_GE(fit->samples, 20u);
    BOOST_CHECK_EQUAL(fit->aircraft, 12u);
    BOOST_CHECK_CLOSE(fit->b, 0.02, 15.0);
}

BOOST_AUTO_TEST_CASE(trackfile_target_gets_fit_estimate)
{
    UatRig rig;
    const auto last = seedUatPopulation(rig, kT0);
    // Qualifier 3: baro only; the aux altitude it would carry must not become a geometric one.
    UatSpec s = uatAirborne(0x123456, 20000, 20400);
    s.qualifier = 3;
    const auto now = last + seconds(1);
    rig.frame(s, now);
    const skycue::TrackId id{0x123456, skycue::AddressType::tisb_trackfile};
    BOOST_REQUIRE(rig.eng.table.find(id));
    BOOST_CHECK(!rig.seen.back().alt_geom_ft);
    BOOST_CHECK(!rig.seen.back().geom_minus_baro_ft);
    const auto fit = rig.eng.fit.fit(now);
    BOOST_REQUIRE(fit);
    const auto r = rig.eng.resolve(id, now);
    BOOST_CHECK(r.source == GeomSource::estimated);
    BOOST_REQUIRE(r.alt_geom_ft);
    BOOST_CHECK_SMALL(*r.alt_geom_ft - (20000 + fit->predict(20000)), 1.0);
    BOOST_CHECK(r.uncertainty_ft);
    BOOST_CHECK(!r.geom_minus_baro_ft);
}

BOOST_AUTO_TEST_CASE(trackfile_target_without_fit_gets_none)
{
    UatRig rig;
    UatSpec s = uatAirborne(0x123456, 20000, 20400);
    s.qualifier = 3;
    rig.frame(s, kT0);
    const skycue::TrackId id{0x123456, skycue::AddressType::tisb_trackfile};
    const auto r = rig.eng.resolve(id, kT0);
    BOOST_CHECK(r.source == GeomSource::none);
    BOOST_CHECK(!r.alt_geom_ft);
    BOOST_CHECK(!r.uncertainty_ft);
}

BOOST_AUTO_TEST_CASE(uat_geometric_sv_type_reported_without_aux)
{
    UatRig rig;
    const auto last = seedUatPopulation(rig, kT0);  // a fit exists, yet it must not be used
    UatSpec s;
    s.type = 0;  // short frame: no aux
    s.address = 0x777777;
    s.lat = 52.1;
    s.lon = 4.6;
    s.alt_geometric = true;
    s.alt_ft = 5000;
    s.nic = 8;
    s.ns_kt = 100;
    s.ew_kt = 100;
    const auto now = last + seconds(1);
    rig.frame(s, now);
    BOOST_REQUIRE(!rig.seen.empty());
    const auto& o = rig.seen.back();
    BOOST_REQUIRE(o.alt_geom_ft);
    BOOST_CHECK_EQUAL(*o.alt_geom_ft, 5000);
    BOOST_CHECK(!o.alt_baro_ft);
    BOOST_CHECK(!o.geom_minus_baro_ft);
    const skycue::TrackId id{0x777777, skycue::AddressType::icao};
    auto r = rig.eng.resolve(id, now);
    BOOST_CHECK(r.source == GeomSource::reported);
    BOOST_REQUIRE(r.alt_geom_ft);
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, 5000);
    BOOST_CHECK(!r.uncertainty_ft);
    BOOST_CHECK(!r.geom_minus_baro_ft);
    // No baro ever arrives, so once the report is stale there is nothing to estimate from.
    BOOST_CHECK(rig.eng.resolve(id, now + seconds(61)).source == GeomSource::none);
}

BOOST_AUTO_TEST_CASE(readsb_path_unchanged)
{
    // readsb passes dump978's UAT on as ADS-R; alt_geom with alt_baro is the phase 4 shape.
    const auto parsed = parseReadsbLine(
        R"({"now":1700000000.0,"hex":"abc123","type":"adsr_icao","alt_baro":30000,"alt_geom":30525,"lat":52.1,"lon":4.6})");
    BOOST_REQUIRE(parsed);
    BOOST_CHECK(parsed->source == skycue::Source::uat_978);
    BOOST_REQUIRE(parsed->geom_minus_baro_ft);
    BOOST_CHECK_EQUAL(*parsed->geom_minus_baro_ft, 525);
    MiniEngine eng;
    eng.feed(*parsed);
    const skycue::TrackId id{0xABC123, skycue::AddressType::icao};
    auto r = eng.resolve(id, parsed->received_time);
    BOOST_CHECK(r.source == GeomSource::reported);
    BOOST_REQUIRE(r.alt_geom_ft);
    BOOST_CHECK_EQUAL(*r.alt_geom_ft, 30525);
    BOOST_REQUIRE(r.geom_minus_baro_ft);
    BOOST_CHECK_EQUAL(*r.geom_minus_baro_ft, 525);
    BOOST_CHECK_EQUAL(*eng.track(id).geom.baro_at_diff_ft, 30000);
    BOOST_CHECK_EQUAL(eng.fit.sampleCount(), 1u);
}
