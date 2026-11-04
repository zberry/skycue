#define BOOST_TEST_MODULE skycued_stats_store
#include <boost/test/unit_test.hpp>

#include "engine_events.hpp"
#include "stats_store.hpp"
#include "test_slow.hpp"

#include <ftw.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace skycued;
using Clock = std::chrono::system_clock;

namespace {

const ReceiverPosition kRx{38.9, -77.0, 100.0};
// 2026-11-03T12:00:00Z, the start of a minute and of an hour.
constexpr int64_t kT0 = 1793707200;

Clock::time_point at(int64_t t_s) { return Clock::time_point(std::chrono::seconds(t_s)); }

skycue::TrackId icao(uint32_t addr) { return skycue::TrackId{addr, skycue::AddressType::icao}; }

struct Obs {
    TrackObservation obs;
    TrackState state;
};

// A positioned observation of `addr` at `t_s`, and a state whose latest is it.
Obs obsAt(uint32_t addr, int64_t t_s, double lat, double lon)
{
    Obs o;
    o.obs.id = icao(addr);
    o.obs.source = skycue::Source::adsb_1090;
    o.obs.received_time = at(t_s);
    o.obs.position_time = at(t_s);
    o.obs.lat = lat;
    o.obs.lon = lon;
    o.obs.alt_baro_ft = 12000;
    o.obs.input_id = "readsb";
    o.state.id = o.obs.id;
    o.state.source = o.obs.source;
    o.state.latest = o.obs;
    o.state.last_input = o.obs.input_id;
    o.state.first_seen = o.obs.received_time;
    o.state.last_seen = o.obs.received_time;
    return o;
}

// The same, re-synchronising state.latest after the test edits obs.
void sync(Obs& o) { o.state.latest = o.obs; }

FilterResult resultFor(double range, double elevation, Decision d) { return FilterResult{d, range, elevation, false}; }

const FilterResult kSend = resultFor(20.0, 3.0, Decision::send);

void feed(StatsStore& st, const Obs& o, const FilterResult& r = kSend) { st.observe(o.obs, o.state, r); }

uint64_t histTotal(const StatsStore& st, StatsHist h)
{
    uint64_t n = 0;
    for (uint64_t c : st.histogram(h).counts)
        n += c;
    return n;
}

}  // namespace

BOOST_AUTO_TEST_CASE(minute_and_hour_placement)
{
    StatsStore st(kRx);
    feed(st, obsAt(1, kT0, 39.0, -77.0));
    feed(st, obsAt(1, kT0 + 59, 39.0, -77.0));
    BOOST_TEST(st.series(StatsRes::minute).size() == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().observations == 2u);
    BOOST_TEST(st.series(StatsRes::minute).back().start_s == kT0);
    feed(st, obsAt(1, kT0 + 60, 39.0, -77.0));
    BOOST_TEST(st.series(StatsRes::minute).size() == 2u);
    BOOST_TEST(st.series(StatsRes::minute).back().observations == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().start_s == kT0 + 60);
    BOOST_TEST(st.series(StatsRes::hour).size() == 1u);
    BOOST_TEST(st.series(StatsRes::hour).back().observations == 3u);
    BOOST_TEST(st.series(StatsRes::hour).back().start_s == kT0);
    BOOST_TEST(st.sinceS().value() == kT0);
    BOOST_TEST(st.seriesSinceS().value() == kT0);
}

BOOST_AUTO_TEST_CASE(minute_ring_wraps_at_24h)
{
    StatsStore st(kRx);
    for (int64_t m = 0; m < 1441; ++m)
        feed(st, obsAt(1, kT0 + m * 60, 39.0, -77.0));
    const auto& s = st.series(StatsRes::minute);
    BOOST_TEST(s.size() == kStatsMinuteBuckets);
    BOOST_TEST(s.front().start_s == kT0 + 60);
    BOOST_TEST(s.back().start_s == kT0 + 1440 * 60);
}

BOOST_AUTO_TEST_CASE(hour_ring_wraps_at_30d)
{
    StatsStore st(kRx);
    for (int64_t h = 0; h < 721; ++h)
        feed(st, obsAt(1, kT0 + h * 3600, 39.0, -77.0));
    const auto& s = st.series(StatsRes::hour);
    BOOST_TEST(s.size() == kStatsHourBuckets);
    BOOST_TEST(s.front().start_s == kT0 + 3600);
    // Hour-long gaps are shorter than the minute ring: it holds the last 24 h,
    // empty minutes included.
    BOOST_TEST(st.series(StatsRes::minute).size() == kStatsMinuteBuckets);
    BOOST_TEST(st.series(StatsRes::minute).back().start_s == kT0 + 720 * 3600);
}

BOOST_AUTO_TEST_CASE(gap_fills_empty_buckets)
{
    StatsStore st(kRx);
    feed(st, obsAt(1, kT0, 39.0, -77.0));
    feed(st, obsAt(1, kT0 + 300, 39.0, -77.0));
    const auto& s = st.series(StatsRes::minute);
    BOOST_REQUIRE(s.size() == 6u);
    for (std::size_t i = 0; i < s.size(); ++i)
        BOOST_TEST(s[i].start_s == kT0 + int64_t(i) * 60);
    for (std::size_t i = 1; i < 5; ++i) {
        BOOST_TEST(s[i].observations == 0u);
        BOOST_TEST(s[i].aircraft == 0u);
        BOOST_TEST(s[i].by_input.empty());
    }
    BOOST_TEST(s.front().observations == 1u);
    BOOST_TEST(s.back().observations == 1u);
}

BOOST_AUTO_TEST_CASE(one_bucket_late_accepted)
{
    StatsStore st(kRx);
    const int64_t b = kT0 + 600;
    feed(st, obsAt(1, b - 60, 39.0, -77.0));
    feed(st, obsAt(1, b, 39.0, -77.0));
    feed(st, obsAt(2, b - 30, 39.0, -77.0));
    const auto& s = st.series(StatsRes::minute);
    BOOST_REQUIRE(s.size() == 2u);
    BOOST_TEST(s[0].start_s == b - 60);
    BOOST_TEST(s[0].observations == 2u);
    BOOST_TEST(s[0].aircraft == 2u);
    BOOST_TEST(s[1].observations == 1u);
    BOOST_TEST(st.seriesSinceS().value() == b - 60);

    // A late observation when only the newest bucket exists makes the one before it.
    StatsStore one(kRx);
    feed(one, obsAt(1, b, 39.0, -77.0));
    feed(one, obsAt(2, b - 30, 39.0, -77.0));
    BOOST_REQUIRE(one.series(StatsRes::minute).size() == 2u);
    BOOST_TEST(one.series(StatsRes::minute).front().start_s == b - 60);
    BOOST_TEST(one.series(StatsRes::minute).front().observations == 1u);
    // The series now begins at the late time; since stays the first stored value.
    BOOST_TEST(one.seriesSinceS().value() == b - 30);
    BOOST_TEST(one.sinceS().value() == b);
}

BOOST_AUTO_TEST_CASE(older_restarts_series)
{
    StatsStore st(kRx);
    const int64_t b = kT0 + 600;
    feed(st, obsAt(1, b - 300, 39.0, -77.0));
    feed(st, obsAt(1, b, 39.0, -77.0));
    feed(st, obsAt(2, b - 61, 39.0, -77.0));
    BOOST_REQUIRE(st.series(StatsRes::minute).size() == 1u);
    BOOST_TEST(st.series(StatsRes::minute).front().start_s == b - 120);
    BOOST_TEST(st.series(StatsRes::minute).front().observations == 1u);
    BOOST_REQUIRE(st.series(StatsRes::hour).size() == 1u);
    BOOST_TEST(st.series(StatsRes::hour).front().observations == 1u);
    BOOST_TEST(st.seriesSinceS().value() == b - 61);
    BOOST_TEST(st.sinceS().value() == b - 300);
    // The ledger and the histograms keep everything.
    BOOST_TEST(st.aircraftCount() == 2u);
    BOOST_TEST(st.aircraft(icao(1))->observations == 2u);
    BOOST_TEST(histTotal(st, StatsHist::altitude) == 3u);
    BOOST_TEST(histTotal(st, StatsHist::range) == 3u);

    // sent/dropped/tick never restart: older times go into the newest bucket.
    st.sent(icao(1), at(b - 3600));
    st.dropped(icao(1), skycue::DropReason::timeout, std::nullopt, at(b - 3600));
    EngineStats es;
    st.tick(at(b - 3600), es);
    BOOST_TEST(st.series(StatsRes::minute).size() == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().sent_updates == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().drops_timeout == 1u);
    BOOST_TEST(st.seriesSinceS().value() == b - 61);
}

BOOST_AUTO_TEST_CASE(forward_jump_is_not_a_restart)
{
    StatsStore st(kRx);
    feed(st, obsAt(1, kT0 + 30, 39.0, -77.0));
    feed(st, obsAt(1, kT0 + 30 + 3 * 86400, 39.0, -77.0));
    BOOST_TEST(st.series(StatsRes::minute).size() == 1u);
    BOOST_TEST(st.series(StatsRes::hour).size() == 73u);
    BOOST_TEST(st.series(StatsRes::hour).front().start_s == kT0);
    BOOST_TEST(st.series(StatsRes::hour).front().observations == 1u);
    BOOST_TEST(st.seriesSinceS().value() == kT0 + 30);
}

BOOST_AUTO_TEST_CASE(distinct_aircraft_per_bucket)
{
    StatsStore st(kRx);
    feed(st, obsAt(0xa, kT0, 39.0, -77.0));
    feed(st, obsAt(0xa, kT0 + 10, 39.0, -77.0));
    feed(st, obsAt(0xb, kT0 + 20, 39.0, -77.0));
    BOOST_TEST(st.series(StatsRes::minute).back().aircraft == 2u);
    BOOST_TEST(st.series(StatsRes::minute).back().new_aircraft == 2u);
    feed(st, obsAt(0xa, kT0 + 70, 39.0, -77.0));
    BOOST_TEST(st.series(StatsRes::minute).back().aircraft == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().new_aircraft == 0u);
    BOOST_TEST(st.series(StatsRes::hour).back().aircraft == 2u);
    BOOST_TEST(st.series(StatsRes::hour).back().new_aircraft == 2u);
    // Same address, another address type: another aircraft.
    Obs t = obsAt(0xa, kT0 + 80, 39.0, -77.0);
    t.obs.id.type = skycue::AddressType::tisb_trackfile;
    t.state.id = t.obs.id;
    sync(t);
    feed(st, t);
    BOOST_TEST(st.series(StatsRes::minute).back().aircraft == 2u);
    BOOST_TEST(st.series(StatsRes::hour).back().aircraft == 3u);
}

BOOST_AUTO_TEST_CASE(histogram_bin_edges)
{
    BOOST_TEST(histBin(StatsHist::range, 9.99) == 0u);
    BOOST_TEST(histBin(StatsHist::range, 10) == 1u);
    BOOST_TEST(histBin(StatsHist::range, 600) == 49u);
    BOOST_TEST(histBin(StatsHist::altitude, -500) == 0u);
    BOOST_TEST(histBin(StatsHist::altitude, 60000) == 59u);
    BOOST_TEST(histBin(StatsHist::vertical_rate, -8000) == 0u);
    BOOST_TEST(histBin(StatsHist::vertical_rate, 7999) == 31u);
    BOOST_TEST(histBin(StatsHist::vertical_rate, 9000) == 31u);
    BOOST_TEST(histBin(StatsHist::elevation, -3) == 0u);
    BOOST_TEST(histBin(StatsHist::elevation, 90) == 44u);
    BOOST_TEST(histBin(StatsHist::rssi, -50) == 0u);
    BOOST_TEST(histBin(StatsHist::rssi, 0) == 49u);
    BOOST_TEST(histBin(StatsHist::rssi, -0.5) == 49u);
    BOOST_TEST(histSize(StatsHist::range) == 50u);
    BOOST_TEST(histSize(StatsHist::altitude) == 60u);
    BOOST_TEST(histSize(StatsHist::speed) == 40u);
    BOOST_TEST(histSize(StatsHist::vertical_rate) == 32u);
    BOOST_TEST(histSize(StatsHist::elevation) == 45u);
    BOOST_TEST(histSize(StatsHist::rssi) == 50u);

    StatsStore st(kRx);
    const StatsHistogram& vr = st.histogram(StatsHist::vertical_rate);
    BOOST_TEST(vr.start == -8000.0);
    BOOST_TEST(vr.bin == 500.0);
    BOOST_TEST(vr.counts.size() == 32u);
    BOOST_TEST(st.histogram(StatsHist::speed).bin == 20.0);

    // The observation's own fields: on ground is altitude 0; baro rate before geom.
    Obs o = obsAt(1, kT0, 39.0, -77.0);
    o.obs.on_ground = true;
    o.obs.gs_kt = 45.0f;
    o.obs.geom_rate_fpm = 1000.0f;
    o.obs.baro_rate_fpm = -600.0f;
    o.obs.rssi = -12.5;
    sync(o);
    feed(st, o, resultFor(25.0, 1.0, Decision::send));
    BOOST_TEST(st.histogram(StatsHist::altitude).counts[0] == 1u);
    BOOST_TEST(st.histogram(StatsHist::speed).counts[2] == 1u);
    BOOST_TEST(st.histogram(StatsHist::vertical_rate).counts[histBin(StatsHist::vertical_rate, -600)] == 1u);
    BOOST_TEST(st.histogram(StatsHist::rssi).counts[37] == 1u);
    BOOST_TEST(st.histogram(StatsHist::range).counts[2] == 1u);
    BOOST_TEST(st.histogram(StatsHist::elevation).counts[0] == 1u);
    BOOST_TEST(st.rssiByRange(2, 18) == 1u);  // floor((-12.5+50)/2) = 18

    // Without a position: no range/elevation, but the other histograms count.
    Obs np = obsAt(2, kT0 + 1, 39.0, -77.0);
    np.obs.lat.reset();
    np.obs.lon.reset();
    np.obs.position_time.reset();
    sync(np);
    feed(st, np, resultFor(30.0, 2.0, Decision::no_position));
    BOOST_TEST(histTotal(st, StatsHist::range) == 1u);
    BOOST_TEST(histTotal(st, StatsHist::elevation) == 1u);
    BOOST_TEST(histTotal(st, StatsHist::altitude) == 2u);
    BOOST_TEST(st.series(StatsRes::minute).back().positions == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().observations == 2u);
}

BOOST_AUTO_TEST_CASE(coverage_sector_and_band)
{
    BOOST_TEST(coverageSector(0) == 0);
    BOOST_TEST(coverageSector(4.99) == 0);
    BOOST_TEST(coverageSector(5) == 1);
    BOOST_TEST(coverageSector(359.9) == 71);
    BOOST_TEST(coverageSector(360) == 0);
    BOOST_TEST(altitudeBand(std::nullopt, false) == 7);
    BOOST_TEST(altitudeBand(2499, false) == 0);
    BOOST_TEST(altitudeBand(2500, false) == 1);
    BOOST_TEST(altitudeBand(5000, false) == 2);
    BOOST_TEST(altitudeBand(39999, false) == 5);
    BOOST_TEST(altitudeBand(40000, false) == 6);
    BOOST_TEST(altitudeBand(std::nullopt, true) == 0);
    BOOST_TEST(altitudeBand(35000, true) == 0);
    BOOST_TEST(std::string(bandName(0)) == "0-2.5k");
    BOOST_TEST(std::string(bandName(6)) == "40k+");
    BOOST_TEST(std::string(bandName(7)) == "unknown");

    // Due north of the receiver, no altitude: sector 0, band unknown.
    StatsStore st(kRx);
    Obs o = obsAt(1, kT0, 39.2, -77.0);
    o.obs.alt_baro_ft.reset();
    sync(o);
    feed(st, o);
    BOOST_TEST(st.coverage(0, 7).n == 1u);
    // Just west of north: sector 71.
    feed(st, obsAt(2, kT0 + 1, 39.2, -77.001));
    BOOST_TEST(st.coverage(71, 3).n == 1u);  // 12000 ft: band 10-20k
}

BOOST_AUTO_TEST_CASE(coverage_keeps_maximum_holder)
{
    StatsStore st(kRx);
    feed(st, obsAt(1, kT0, 39.5, -77.0), resultFor(40.0, 1.0, Decision::send));
    feed(st, obsAt(2, kT0 + 5, 39.9, -77.0), resultFor(60.0, 1.0, Decision::send));
    feed(st, obsAt(3, kT0 + 9, 39.2, -77.0), resultFor(20.0, 1.0, Decision::send));
    const CoverageCell& c = st.coverage(0, 3);
    BOOST_TEST(c.n == 3u);
    BOOST_TEST(c.max_range_nm == 60.0);
    BOOST_REQUIRE(c.id.has_value());
    BOOST_TEST(c.id->address == 2u);
    BOOST_TEST(c.time_s == kT0 + 5);
    // The bucket's maximum too.
    BOOST_TEST(st.series(StatsRes::minute).back().max_range_nm == 60.0);
    BOOST_TEST(st.series(StatsRes::minute).back().max_range_id->address == 2u);
}

BOOST_AUTO_TEST_CASE(no_receiver_no_coverage)
{
    StatsStore st(std::nullopt);
    BOOST_TEST(!st.receiver().has_value());
    for (uint32_t i = 0; i < 10; ++i)
        feed(st, obsAt(i + 1, kT0 + i, 39.0 + 0.1 * i, -77.0), resultFor(10.0 * i, 2.0, Decision::send));
    for (int s = 0; s < kStatsSectors; ++s)
        for (int b = 0; b < kStatsBands; ++b)
            BOOST_TEST(st.coverage(s, b).n == 0u);
    BOOST_TEST(histTotal(st, StatsHist::range) == 0u);
    BOOST_TEST(histTotal(st, StatsHist::elevation) == 0u);
    BOOST_TEST(histTotal(st, StatsHist::altitude) == 10u);
    BOOST_TEST(st.series(StatsRes::minute).back().max_range_nm == 0.0);
    BOOST_TEST(!st.aircraft(icao(5))->max_range_nm.has_value());
    // The grid needs no receiver.
    BOOST_TEST(st.gridCells().size() == 10u);
}

BOOST_AUTO_TEST_CASE(grid_cap_ignores_new_cells)
{
    StatsLimits lim;
    lim.grid_cells_max = 10;
    StatsStore st(kRx, lim);
    for (int i = 0; i < 11; ++i)
        feed(st, obsAt(1, kT0 + i, 38.0 + 0.05 * i + 0.01, -77.0));
    auto cells = st.gridCells();
    BOOST_TEST(cells.size() == 10u);
    BOOST_TEST(st.gridFull());
    // Sorted by lat_i: the first ten, the eleventh was ignored.
    for (std::size_t i = 1; i < cells.size(); ++i)
        BOOST_TEST(cells[i - 1].lat_i < cells[i].lat_i);
    BOOST_TEST(cells.front().lat_i == int32_t(std::floor(38.01 / kStatsGridCellDeg)));
    BOOST_TEST(cells.front().lon_i == int32_t(std::floor(-77.0 / kStatsGridCellDeg)));
    feed(st, obsAt(1, kT0 + 20, 38.02, -77.0));
    cells = st.gridCells();
    BOOST_TEST(cells.size() == 10u);
    BOOST_TEST(cells.front().n == 2u);

    StatsStore roomy(kRx);
    feed(roomy, obsAt(1, kT0, 38.01, -77.0));
    BOOST_TEST(!roomy.gridFull());
    // Negative latitude/longitude cells: floor, not truncation.
    feed(roomy, obsAt(1, kT0, -0.01, -0.01));
    cells = roomy.gridCells();
    BOOST_REQUIRE(cells.size() == 2u);
    BOOST_TEST(cells.front().lat_i == -1);
    BOOST_TEST(cells.front().lon_i == -1);
}

BOOST_AUTO_TEST_CASE(categorical_counts)
{
    StatsStore st(kRx);
    Obs a = obsAt(1, kT0, 39.0, -77.0);
    a.obs.category = std::string("A3");
    sync(a);
    feed(st, a, resultFor(20.0, 3.0, Decision::send));
    Obs z = obsAt(2, kT0 + 1, 39.0, -77.0);
    z.obs.category = std::string("Z9");
    z.obs.source = skycue::Source::uat_978;
    z.obs.id.type = skycue::AddressType::non_icao;
    z.state.id = z.obs.id;
    sync(z);
    feed(st, z, resultFor(20.0, 3.0, Decision::out_of_range));
    const auto& cat = st.counts(StatsCount::category);
    BOOST_TEST(cat.size() == 2u);
    BOOST_TEST(cat.at("A3") == 1u);
    BOOST_TEST(cat.at("none") == 1u);
    BOOST_TEST(st.counts(StatsCount::source).at("adsb_1090") == 1u);
    BOOST_TEST(st.counts(StatsCount::source).at("uat_978") == 1u);
    BOOST_TEST(st.counts(StatsCount::addr_type).at("icao") == 1u);
    BOOST_TEST(st.counts(StatsCount::addr_type).at("non_icao") == 1u);
    BOOST_TEST(st.counts(StatsCount::decision).at("send") == 1u);
    BOOST_TEST(st.counts(StatsCount::decision).at("out_of_range") == 1u);
    BOOST_TEST(st.counts(StatsCount::input).at("readsb") == 2u);
    BOOST_TEST(st.uatSeen());
    BOOST_TEST(st.series(StatsRes::minute).back().emergencies == 0u);

    Obs e = obsAt(3, kT0 + 2, 39.0, -77.0);
    e.obs.squawk = std::string("7700");
    sync(e);
    feed(st, e);
    BOOST_TEST(st.counts(StatsCount::squawk_emergency).at("7700") == 1u);
    BOOST_TEST(st.counts(StatsCount::squawk_emergency).size() == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().emergencies == 1u);
    BOOST_TEST(st.aircraft(icao(3))->emergencies == 1u);
    // An emergency flag other than "none" counts too; "none" does not.
    Obs f = obsAt(4, kT0 + 3, 39.0, -77.0);
    f.obs.emergency = std::string("none");
    sync(f);
    feed(st, f);
    BOOST_TEST(st.series(StatsRes::minute).back().emergencies == 1u);
    f.obs.emergency = std::string("lifeguard");
    sync(f);
    feed(st, f);
    BOOST_TEST(st.series(StatsRes::minute).back().emergencies == 2u);
    BOOST_TEST(st.counts(StatsCount::squawk_emergency).size() == 1u);
}

BOOST_AUTO_TEST_CASE(by_input_capped_at_16)
{
    StatsStore st(kRx);
    for (int i = 0; i < 17; ++i) {
        Obs o = obsAt(1, kT0 + i, 39.0, -77.0);
        o.obs.input_id = "in" + std::to_string(i);
        sync(o);
        feed(st, o);
    }
    Obs u = obsAt(1, kT0 + 30, 39.0, -77.0);
    u.obs.input_id.clear();
    sync(u);
    feed(st, u);
    const auto& b = st.series(StatsRes::minute).back().by_input;
    BOOST_TEST(b.size() == 17u);
    BOOST_TEST(b.at("other") == 2u);  // in16 and the unknown one
    BOOST_TEST(b.count("in16") == 0u);
    BOOST_TEST(b.at("in0") == 1u);
    BOOST_TEST(st.series(StatsRes::hour).back().by_input.size() == 17u);
    const auto& c = st.counts(StatsCount::input);
    BOOST_TEST(c.size() == 17u);
    BOOST_TEST(c.at("other") == 2u);

    // A fresh store names an input-less observation "unknown".
    StatsStore fresh(kRx);
    feed(fresh, u);
    BOOST_TEST(fresh.series(StatsRes::minute).back().by_input.at("unknown") == 1u);
}

BOOST_AUTO_TEST_CASE(ledger_fields)
{
    StatsStore st(kRx);
    const char* calls[] = {"A", "B", "C", "D", "E", "B"};
    int64_t t = kT0;
    for (int i = 0; i < 6; ++i) {
        Obs o = obsAt(7, t, 39.0, -77.0);
        o.obs.callsign = std::string(calls[i]);
        o.obs.alt_baro_ft = 10000 + 1000 * i;
        o.obs.gs_kt = float(300 + 10 * i);
        o.obs.rssi = -20.0 + i;
        o.obs.squawk = std::string("1200");
        o.obs.registration = std::string("N12345");
        o.obs.category = std::string("A3");
        o.obs.input_id = i % 2 ? "b" : "a";
        sync(o);
        feed(st, o, resultFor(i == 3 ? 80.0 : 10.0, 1.0, Decision::send));
        t += i == 0 ? 11 * 60 : 9 * 60;  // one gap over 10 min, then 9 min gaps
    }
    const AircraftStats* a = st.aircraft(icao(7));
    BOOST_REQUIRE(a != nullptr);
    BOOST_TEST(a->sessions == 2u);
    BOOST_TEST(a->observations == 6u);
    BOOST_TEST(a->positions == 6u);
    BOOST_TEST(a->first_seen_s == kT0);
    BOOST_TEST(a->last_seen_s == kT0 + 11 * 60 + 4 * 9 * 60);
    const std::vector<std::string> want{"C", "D", "E", "B"};
    BOOST_TEST(a->callsigns == want, boost::test_tools::per_element());
    BOOST_TEST(a->squawks.size() == 1u);
    BOOST_TEST(a->inputs.size() == 2u);
    BOOST_TEST(a->inputs.back() == "b");
    BOOST_TEST(a->min_alt_ft.value() == 10000);
    BOOST_TEST(a->max_alt_ft.value() == 15000);
    BOOST_TEST(a->max_gs_kt.value() == 350.0);
    BOOST_TEST(a->min_rssi.value() == -20.0);
    BOOST_TEST(a->max_rssi.value() == -15.0);
    BOOST_TEST(a->max_range_nm.value() == 80.0);
    BOOST_TEST(a->max_range_time_s == kT0 + 11 * 60 + 2 * 9 * 60);
    BOOST_TEST(a->max_range_lat == 39.0);
    BOOST_TEST(a->max_range_lon == -77.0);
    BOOST_TEST(a->max_range_bearing_deg < 0.001);  // due north of the receiver
    BOOST_TEST(a->registration.value() == "N12345");
    BOOST_TEST(a->category.value() == "A3");
    BOOST_TEST(int(a->source) == int(skycue::Source::adsb_1090));

    // Nine minutes apart only: one session.
    StatsStore one(kRx);
    feed(one, obsAt(8, kT0, 39.0, -77.0));
    feed(one, obsAt(8, kT0 + 9 * 60, 39.0, -77.0));
    BOOST_TEST(one.aircraft(icao(8))->sessions == 1u);
    BOOST_TEST(one.aircraft(icao(9)) == nullptr);
}

BOOST_AUTO_TEST_CASE(sent_and_drops_counted)
{
    StatsStore st(kRx);
    feed(st, obsAt(1, kT0, 39.0, -77.0));
    st.sent(icao(1), at(kT0 + 1));
    BOOST_TEST(st.series(StatsRes::minute).back().sent_updates == 1u);
    BOOST_TEST(st.series(StatsRes::hour).back().sent_updates == 1u);
    BOOST_TEST(st.aircraft(icao(1))->sent_updates == 1u);
    st.dropped(icao(1), skycue::DropReason::filtered, Decision::out_of_range, at(kT0 + 2));
    const StatsBucket& b = st.series(StatsRes::minute).back();
    BOOST_TEST(b.drops_filtered == 1u);
    BOOST_TEST(b.drops_out_of_range == 1u);
    BOOST_TEST(b.drops_timeout == 0u);
    BOOST_TEST(st.series(StatsRes::hour).back().drops_out_of_range == 1u);
    BOOST_TEST(st.aircraft(icao(1))->drops == 1u);
    st.dropped(icao(99), skycue::DropReason::source_lost, std::nullopt, at(kT0 + 3));
    st.dropped(icao(99), skycue::DropReason::timeout, std::nullopt, at(kT0 + 3));
    BOOST_TEST(st.series(StatsRes::minute).back().drops_source_lost == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().drops_timeout == 1u);
    BOOST_TEST(st.aircraft(icao(99)) == nullptr);
    BOOST_TEST(st.aircraftCount() == 1u);
    // A sent in a later minute advances the series.
    st.sent(icao(1), at(kT0 + 61));
    BOOST_TEST(st.series(StatsRes::minute).size() == 2u);
    BOOST_TEST(st.series(StatsRes::minute).back().sent_updates == 1u);
}

BOOST_AUTO_TEST_CASE(eviction_at_cap)
{
    StatsLimits lim;
    lim.aircraft_max = 100;
    StatsStore st(kRx, lim);
    for (uint32_t i = 0; i < 101; ++i)
        feed(st, obsAt(i + 1, kT0 + i, 39.0, -77.0));
    BOOST_TEST(st.aircraftCount() == 96u);
    BOOST_TEST(st.evicted() == 5u);
    for (uint32_t i = 1; i <= 5; ++i)
        BOOST_TEST(st.aircraft(icao(i)) == nullptr);
    BOOST_TEST(st.aircraft(icao(6)) != nullptr);
    BOOST_TEST(st.aircraft(icao(101)) != nullptr);
    std::size_t seen = 0;
    st.forEachAircraft([&](const AircraftStats& a) {
        ++seen;
        BOOST_TEST(a.id.address > 5u);
    });
    BOOST_TEST(seen == 96u);

    // A tiny cap still evicts at least one.
    StatsLimits tiny;
    tiny.aircraft_max = 3;
    StatsStore small(kRx, tiny);
    for (uint32_t i = 0; i < 4; ++i)
        feed(small, obsAt(i + 1, kT0 + i, 39.0, -77.0));
    BOOST_TEST(small.aircraftCount() == 3u);
    BOOST_TEST(small.evicted() == 1u);
    BOOST_TEST(small.aircraft(icao(1)) == nullptr);
}

BOOST_AUTO_TEST_CASE(eviction_keeps_observed_aircraft)
{
    StatsLimits lim;
    lim.aircraft_max = 100;
    StatsStore st(kRx, lim);
    for (uint32_t i = 0; i < 100; ++i)
        feed(st, obsAt(i + 1, kT0 + 1000 + i, 39.0, -77.0));
    // The data clock behind the ledger: older than every entry's last_seen.
    feed(st, obsAt(999, kT0, 39.0, -77.0));
    BOOST_TEST(st.aircraftCount() == 96u);
    BOOST_TEST(st.evicted() == 5u);
    BOOST_REQUIRE(st.aircraft(icao(999)) != nullptr);
    for (uint32_t i = 1; i <= 5; ++i)
        BOOST_TEST(st.aircraft(icao(i)) == nullptr);
    BOOST_TEST(st.series(StatsRes::minute).back().new_aircraft == 1u);
    // The next sighting is not new again.
    feed(st, obsAt(999, kT0 + 1, 39.0, -77.0));
    BOOST_TEST(st.series(StatsRes::minute).back().new_aircraft == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().aircraft == 1u);
    BOOST_TEST(st.aircraft(icao(999))->observations == 2u);
    BOOST_TEST(st.evicted() == 5u);
}

BOOST_AUTO_TEST_CASE(huge_values_are_clamped)
{
    StatsStore st(kRx);
    // Off the globe or absurd: no position anywhere (grid, coverage, range).
    for (double bad : {1e300, -1e300, 91.0}) {
        feed(st, obsAt(1, kT0, bad, -77.0), resultFor(10.0, 1.0, Decision::send));
        feed(st, obsAt(1, kT0, 39.0, bad * 2.0), resultFor(10.0, 1.0, Decision::send));
    }
    BOOST_TEST(st.gridCells().empty());
    BOOST_TEST(st.series(StatsRes::minute).back().positions == 0u);
    BOOST_TEST(st.series(StatsRes::minute).back().observations == 6u);
    BOOST_TEST(histTotal(st, StatsHist::range) == 0u);
    BOOST_TEST(st.aircraft(icao(1))->positions == 0u);
    BOOST_TEST(!st.aircraft(icao(1))->max_range_nm.has_value());

    // Huge finite range and rssi land in the edge bins.
    Obs o = obsAt(2, kT0 + 1, 39.5, -77.0);
    o.obs.rssi = 1e300;
    o.obs.gs_kt = 1e30f;
    sync(o);
    feed(st, o, resultFor(1e300, 1e300, Decision::send));
    BOOST_TEST(st.rssiByRange(kStatsRssiRangeBins - 1, kStatsRssiBins - 1) == 1u);
    BOOST_TEST(st.histogram(StatsHist::range).counts.back() == 1u);
    BOOST_TEST(st.histogram(StatsHist::elevation).counts.back() == 1u);
    BOOST_TEST(st.histogram(StatsHist::rssi).counts.back() == 1u);
    BOOST_TEST(st.histogram(StatsHist::speed).counts.back() == 1u);
    o.obs.rssi = -1e300;
    sync(o);
    feed(st, o, resultFor(-1e300, -1e300, Decision::send));
    BOOST_TEST(st.rssiByRange(0, 0) == 1u);
    BOOST_TEST(st.histogram(StatsHist::range).counts.front() == 1u);
    BOOST_TEST(st.histogram(StatsHist::rssi).counts.front() == 1u);
    BOOST_TEST(st.gridCells().size() == 1u);
}

BOOST_AUTO_TEST_CASE(nan_fields_are_ignored)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    StatsStore st(kRx);
    Obs o = obsAt(5, kT0, 39.5, -77.0);
    o.obs.rssi = nan;
    o.obs.gs_kt = std::numeric_limits<float>::quiet_NaN();
    o.obs.baro_rate_fpm = std::numeric_limits<float>::quiet_NaN();
    // An integer altitude cannot be NaN: the altitude is simply absent.
    o.obs.alt_baro_ft.reset();
    o.obs.alt_geom_ft.reset();
    sync(o);
    BOOST_CHECK_NO_THROW(feed(st, o, resultFor(nan, nan, Decision::send)));
    for (StatsHist h : {StatsHist::range, StatsHist::altitude, StatsHist::speed, StatsHist::vertical_rate,
                        StatsHist::elevation, StatsHist::rssi})
        BOOST_TEST(histTotal(st, h) == 0u);
    for (int s = 0; s < kStatsSectors; ++s)
        for (int b = 0; b < kStatsBands; ++b)
            BOOST_TEST(st.coverage(s, b).n == 0u);
    const AircraftStats* a = st.aircraft(icao(5));
    BOOST_REQUIRE(a != nullptr);
    BOOST_TEST(a->observations == 1u);
    BOOST_TEST(a->positions == 1u);
    BOOST_TEST(!a->min_rssi.has_value());
    BOOST_TEST(!a->max_gs_kt.has_value());
    BOOST_TEST(!a->max_range_nm.has_value());
    BOOST_TEST(!a->min_alt_ft.has_value());
    BOOST_TEST(st.series(StatsRes::minute).back().observations == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().max_range_nm == 0.0);
    BOOST_TEST(st.gridCells().size() == 1u);
}

BOOST_AUTO_TEST_CASE(tick_deltas_and_samples)
{
    StatsStore st(kRx);
    EngineStats s;
    // No counts and no series: a tick stores nothing (client samples alone
    // do not start the series).
    s.tcp_clients = 2;
    st.tick(at(kT0), s);
    BOOST_TEST(st.series(StatsRes::minute).empty());
    BOOST_TEST(!st.uatSeen());
    BOOST_TEST(!st.dirty());
    // The bases start at 0 (the Engine's counters at its start), so the
    // first count is kept whole, as from an unpaced replay before any tick.
    s.uat_downlink = 10;
    st.tick(at(kT0 + 1), s);
    BOOST_REQUIRE(st.series(StatsRes::minute).size() == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().uat_downlink == 10u);
    BOOST_TEST(st.series(StatsRes::minute).back().tcp_clients == 2u);
    BOOST_TEST(st.uatSeen());
    BOOST_TEST(st.dirty());
    s.uat_downlink = 15;
    st.tick(at(kT0 + 1), s);
    BOOST_TEST(st.series(StatsRes::minute).back().uat_downlink == 15u);
    s.uat_downlink = 3;  // the counter went down: a new base
    st.tick(at(kT0 + 2), s);
    BOOST_TEST(st.series(StatsRes::minute).back().uat_downlink == 15u);
    s.uat_downlink = 4;
    s.uat_uplink = 0;
    s.fisb_products = 0;
    st.tick(at(kT0 + 3), s);
    BOOST_TEST(st.series(StatsRes::minute).back().uat_downlink == 16u);

    // Uplinks and products count the same way.
    s.uat_uplink = 7;
    s.fisb_products = 2;
    st.tick(at(kT0 + 4), s);
    BOOST_TEST(st.series(StatsRes::minute).back().uat_uplink == 7u);
    BOOST_TEST(st.series(StatsRes::minute).back().fisb_products == 2u);

    // Client samples: the minute holds the latest, the hour the maximum.
    st.markSaved();
    s.tcp_clients = 5;
    s.web_clients = 3;
    st.tick(at(kT0 + 5), s);
    BOOST_TEST(st.dirty());
    s.tcp_clients = 1;
    s.web_clients = 1;
    st.tick(at(kT0 + 6), s);
    BOOST_TEST(st.series(StatsRes::minute).back().tcp_clients == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().web_clients == 1u);
    BOOST_TEST(st.series(StatsRes::hour).back().tcp_clients == 5u);
    BOOST_TEST(st.series(StatsRes::hour).back().web_clients == 3u);

    // The same sample again changes nothing: not dirty.
    st.markSaved();
    st.tick(at(kT0 + 7), s);
    BOOST_TEST(!st.dirty());
    // Advancing into a new, empty minute is not a change either.
    s.tcp_clients = 0;
    s.web_clients = 0;
    st.tick(at(kT0 + 130), s);
    BOOST_TEST(st.series(StatsRes::minute).size() == 3u);
    BOOST_TEST(!st.dirty());
}

BOOST_AUTO_TEST_CASE(totals_over_range)
{
    StatsStore st(kRx);
    feed(st, obsAt(1, kT0, 39.0, -77.0), resultFor(30.0, 1.0, Decision::send));
    st.sent(icao(1), at(kT0 + 1));
    feed(st, obsAt(2, kT0 + 60, 39.0, -77.0), resultFor(90.0, 1.0, Decision::send));
    st.dropped(icao(2), skycue::DropReason::timeout, std::nullopt, at(kT0 + 61));
    st.dropped(icao(2), skycue::DropReason::shutdown, std::nullopt, at(kT0 + 61));
    feed(st, obsAt(3, kT0 + 120, 39.0, -77.0), resultFor(50.0, 1.0, Decision::send));
    st.sent(icao(3), at(kT0 + 121));
    st.dropped(icao(3), skycue::DropReason::filtered, Decision::blocked, at(kT0 + 122));

    SeriesTotals all = st.totals(StatsRes::minute, kT0, kT0 + 120);
    BOOST_TEST(all.observations == 3u);
    BOOST_TEST(all.positions == 3u);
    BOOST_TEST(all.new_aircraft == 3u);
    BOOST_TEST(all.sent_updates == 2u);
    BOOST_TEST(all.drops == 2u);
    BOOST_TEST(all.max_range_nm == 90.0);
    BOOST_TEST(all.max_range_id->address == 2u);

    SeriesTotals last = st.totals(StatsRes::minute, kT0 + 61, kT0 + 1000);
    BOOST_TEST(last.observations == 1u);
    BOOST_TEST(last.drops == 1u);
    BOOST_TEST(last.max_range_nm == 50.0);
    BOOST_TEST(last.max_range_id->address == 3u);

    SeriesTotals none = st.totals(StatsRes::minute, kT0 + 1000, kT0 + 2000);
    BOOST_TEST(none.observations == 0u);
    BOOST_TEST(!none.max_range_id.has_value());

    SeriesTotals hour = st.totals(StatsRes::hour, kT0, kT0);
    BOOST_TEST(hour.observations == 3u);
    BOOST_TEST(hour.max_range_id->address == 2u);
}

BOOST_AUTO_TEST_CASE(reset_empties_everything)
{
    StatsStore st(kRx);
    for (uint32_t i = 0; i < 5; ++i) {
        Obs o = obsAt(i + 1, kT0 + 60 * i, 39.5, -77.0);
        o.obs.source = skycue::Source::uat_978;
        sync(o);
        feed(st, o);
    }
    st.sent(icao(1), at(kT0 + 300));
    BOOST_TEST(st.uatSeen());
    st.markSaved();
    st.reset(at(kT0 + 400));
    BOOST_TEST(st.dirty());
    BOOST_TEST(st.series(StatsRes::minute).empty());
    BOOST_TEST(st.series(StatsRes::hour).empty());
    for (StatsHist h : {StatsHist::range, StatsHist::altitude, StatsHist::speed, StatsHist::vertical_rate,
                        StatsHist::elevation, StatsHist::rssi}) {
        BOOST_TEST(histTotal(st, h) == 0u);
        BOOST_TEST(st.histogram(h).counts.size() == histSize(h));
    }
    for (int s = 0; s < kStatsSectors; ++s)
        BOOST_TEST(st.coverage(s, 3).n == 0u);
    BOOST_TEST(st.gridCells().empty());
    BOOST_TEST(st.counts(StatsCount::source).empty());
    BOOST_TEST(st.aircraftCount() == 0u);
    BOOST_TEST(st.evicted() == 0u);
    BOOST_TEST(st.addressSetEntries() == 0u);
    BOOST_TEST(!st.uatSeen());
    BOOST_TEST(st.sinceS().value() == kT0 + 400);
    BOOST_TEST(st.seriesSinceS().value() == kT0 + 400);
    // Observing again starts the series without a restart.
    feed(st, obsAt(1, kT0 + 410, 39.5, -77.0));
    BOOST_TEST(st.series(StatsRes::minute).size() == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().new_aircraft == 1u);
    BOOST_TEST(st.sinceS().value() == kT0 + 400);
}

BOOST_AUTO_TEST_CASE(memory_bounds_hold)
{
    StatsStore st(kRx);  // default limits: aircraft_max 20000
    const int64_t minutes = 31 * 1440;
    std::size_t max_sets = 0;
    for (int64_t m = 0; m < minutes; ++m) {
        Obs o = obsAt(uint32_t(m % 30000) + 1, kT0 + m * 60, 39.0 + 0.001 * double(m % 500), -77.0);
        o.obs.input_id = "in" + std::to_string(m % 17);
        o.obs.callsign = "C" + std::to_string(m % 7);
        o.obs.squawk = std::to_string(1000 + m % 9);
        sync(o);
        feed(st, o);
        if (m % 997 == 0) {
            BOOST_TEST(st.series(StatsRes::minute).size() <= kStatsMinuteBuckets);
            BOOST_TEST(st.series(StatsRes::hour).size() <= kStatsHourBuckets);
            BOOST_TEST(st.aircraftCount() <= 20000u);
        }
        if (st.addressSetEntries() > max_sets)
            max_sets = st.addressSetEntries();
    }
    BOOST_TEST(st.series(StatsRes::minute).size() == kStatsMinuteBuckets);
    BOOST_TEST(st.series(StatsRes::hour).size() == kStatsHourBuckets);
    BOOST_TEST(st.aircraftCount() <= 20000u);
    BOOST_TEST(st.evicted() > 0u);
    bool lists_ok = true;
    st.forEachAircraft([&](const AircraftStats& a) {
        if (a.callsigns.size() > kStatsListMax || a.squawks.size() > kStatsListMax || a.inputs.size() > kStatsListMax)
            lists_ok = false;
    });
    BOOST_TEST(lists_ok);
    bool inputs_ok = true;
    for (const auto& b : st.series(StatsRes::minute))
        inputs_ok = inputs_ok && b.by_input.size() <= kStatsInputsMax + 1;
    for (const auto& b : st.series(StatsRes::hour))
        inputs_ok = inputs_ok && b.by_input.size() <= kStatsInputsMax + 1;
    BOOST_TEST(inputs_ok);
    BOOST_TEST(st.counts(StatsCount::input).size() <= kStatsInputsMax + 1);
    // One observation a minute: the open sets hold two minutes' ids (2) plus
    // two hours' (120).
    BOOST_TEST(max_sets <= 2u + 120u);
    BOOST_TEST(st.gridCells().size() <= StatsLimits().grid_cells_max);

    // Two idle hours of ticks discard every set.
    EngineStats es;
    const int64_t end = kT0 + minutes * 60;
    for (int64_t s = 0; s <= 2 * 3600; s += 60)
        st.tick(at(end + s), es);
    BOOST_TEST(st.addressSetEntries() == 0u);
}

BOOST_AUTO_TEST_CASE(aggregates_100k_observations_fast)
{
    StatsStore st(kRx);
    std::vector<Obs> pre;
    pre.reserve(5000);
    for (uint32_t a = 0; a < 5000; ++a) {
        // Spread within ~250 nm (about 4 degrees) of the receiver.
        const double lat = 38.9 + 4.0 * std::sin(double(a)) * 0.98;
        const double lon = -77.0 + 4.0 * std::cos(double(a) * 1.3) * 0.98;
        Obs o = obsAt(a + 1, kT0, lat, lon);
        o.obs.gs_kt = float(a % 500);
        o.obs.rssi = -30.0 + double(a % 25);
        o.obs.callsign = "CS" + std::to_string(a % 50);
        o.obs.input_id = a % 2 ? "readsb" : "dump978";
        sync(o);
        pre.push_back(o);
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) {
        Obs& o = pre[std::size_t(i) % pre.size()];
        o.obs.received_time = at(kT0 + i / 50);
        o.state.latest.received_time = o.obs.received_time;
        st.observe(o.obs, o.state, resultFor(double(i % 250), 2.0, Decision::send));
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    BOOST_TEST_MESSAGE("100000 observations aggregated in " << ms << " ms");
    BOOST_TEST(ms < 1000 * testSlowFactor());
    BOOST_TEST(st.aircraftCount() == 5000u);
    BOOST_TEST(st.totals(StatsRes::hour, kT0, kT0 + 86400).observations == 100000u);
}

// ---------------------------------------------------------------------------
// The stats file (Task 2): toJson / fromJson, loadStatsFile, saveStatsFile.

namespace {

int removeStatsEntry(const char* p, const struct stat*, int, struct FTW*) { return ::remove(p); }

struct StatsDir {
    std::string path;
    StatsDir()
    {
        const char* base = std::getenv("TMPDIR");
        std::string tmpl = std::string(base && *base ? base : "/tmp") + "/skycue-stats-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (!::mkdtemp(buf.data()))
            throw std::runtime_error("mkdtemp failed");
        path = buf.data();
    }
    ~StatsDir() { ::nftw(path.c_str(), removeStatsEntry, 16, FTW_DEPTH | FTW_PHYS); }
    StatsDir(const StatsDir&) = delete;
    StatsDir& operator=(const StatsDir&) = delete;
};

bool fileExists(const std::string& p)
{
    struct stat sb {};
    return ::stat(p.c_str(), &sb) == 0;
}

void writeText(const std::string& p, const std::string& text)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string readText(const std::string& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A store fed every kind of event: two aircraft with positions and identity
// fields, one without a position, sends, drops (filtered with a decision and
// timeout) and ticks with client samples and UAT deltas, over three hours.
StatsStore populatedStore()
{
    StatsStore st(kRx);
    Obs a = obsAt(0xa4f2c1, kT0 + 5, 39.2, -76.8);
    a.obs.callsign = "UAL123";
    a.obs.squawk = "1200";
    a.obs.registration = "N123UA";
    a.obs.category = "A3";
    a.obs.gs_kt = 412.3f;
    a.obs.baro_rate_fpm = -640.0f;
    a.obs.rssi = -12.3;
    sync(a);
    feed(st, a, resultFor(23.456789012345, 1.25, Decision::send));
    a.obs.received_time = at(kT0 + 70);
    a.obs.callsign = "UAL124";
    a.obs.input_id = "radar";
    a.obs.lat = 39.31234567;
    a.obs.lon = -76.71234567;
    sync(a);
    feed(st, a, resultFor(25.1, 1.5, Decision::send));

    Obs b = obsAt(0x00abcd, kT0 + 80, 38.5, -77.3);
    b.obs.id.type = skycue::AddressType::non_icao;
    b.state.id = b.obs.id;
    b.obs.source = skycue::Source::uat_978;
    b.obs.input_id = "dump978";
    b.obs.squawk = "7700";
    b.obs.emergency = "general";
    b.obs.alt_baro_ft.reset();
    b.obs.on_ground = true;
    b.obs.geom_rate_fpm = 0.0f;
    sync(b);
    feed(st, b, resultFor(31.5, 0.5, Decision::out_of_range));

    Obs c = obsAt(0x123456, kT0 + 2 * 3600 + 10, 0, 0);
    c.obs.lat.reset();
    c.obs.lon.reset();
    c.obs.alt_baro_ft.reset();
    sync(c);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    feed(st, c, resultFor(nan, nan, Decision::no_position));

    st.sent(a.obs.id, at(kT0 + 2 * 3600 + 11));
    st.dropped(b.obs.id, skycue::DropReason::filtered, Decision::out_of_range, at(kT0 + 2 * 3600 + 12));
    st.dropped(a.obs.id, skycue::DropReason::timeout, std::nullopt, at(kT0 + 2 * 3600 + 13));
    EngineStats es;
    es.tcp_clients = 2;
    es.web_clients = 1;
    st.tick(at(kT0 + 2 * 3600 + 14), es);
    es.uat_downlink = 5;
    es.uat_uplink = 3;
    es.fisb_products = 7;
    es.tcp_clients = 3;
    st.tick(at(kT0 + 2 * 3600 + 15), es);
    return st;
}

// Replaces (or, with drop, removes) the first element of the array `array`
// (e.g. "\"observations\":[") found after `within` in `text`.
std::string editFirstElement(const std::string& text, const char* within, const char* array,
                             const std::string& replacement, bool drop = false)
{
    const std::size_t w = text.find(within);
    if (w == std::string::npos)
        throw std::runtime_error(std::string("no ") + within);
    const std::size_t a = text.find(array, w);
    if (a == std::string::npos)
        throw std::runtime_error(std::string("no ") + array);
    const std::size_t b = a + std::string(array).size();
    const std::size_t e = text.find_first_of(",]", b);
    std::string out = text;
    if (drop)
        out.erase(b, e - b + (text[e] == ',' ? 1 : 0));
    else
        out.replace(b, e - b, replacement);
    return out;
}

// Prepends one bucket (start first_start - 60, zero counters) to every
// column of the "minutes" object.
std::string prependMinute(const std::string& text, int64_t first_start)
{
    const std::size_t from = text.find("\"minutes\":{");
    const std::size_t to = text.find("\"hours\":", from);
    if (from == std::string::npos || to == std::string::npos)
        throw std::runtime_error("no minutes/hours");
    std::string res = text.substr(0, from);
    for (std::size_t i = from; i < to; ++i) {
        res += text[i];
        if (text[i] != '[')
            continue;
        const std::size_t q2 = i - 2;  // "<key>":[
        const std::size_t q1 = text.rfind('"', q2 - 1);
        const std::string key = text.substr(q1 + 1, q2 - q1 - 1);
        if (key == "start")
            res += std::to_string(first_start - 60) + ",";
        else if (key == "max_range_key")
            res += "\"\",";
        else
            res += "0,";
    }
    res += text.substr(to);
    return res;
}

uint64_t coverageTotal(const StatsStore& st)
{
    uint64_t n = 0;
    for (int s = 0; s < kStatsSectors; ++s)
        for (int b = 0; b < kStatsBands; ++b)
            n += st.coverage(s, b).n;
    return n;
}

}  // namespace

BOOST_AUTO_TEST_CASE(round_trip_is_identical)
{
    const StatsStore st = populatedStore();
    const std::string text = st.toJson();
    const StatsStore back = StatsStore::fromJson(text, kRx, StatsLimits());
    BOOST_TEST(back.toJson() == text);
    BOOST_TEST(!back.dirty());
    BOOST_TEST(back.addressSetEntries() == 0u);

    // One ledger entry, field by field.
    const AircraftStats* a = back.aircraft(icao(0xa4f2c1));
    BOOST_REQUIRE(a);
    BOOST_TEST(a->first_seen_s == kT0 + 5);
    BOOST_TEST(a->last_seen_s == kT0 + 70);
    BOOST_TEST(a->observations == 2u);
    BOOST_TEST(a->positions == 2u);
    BOOST_TEST(a->sessions == 1u);
    BOOST_TEST(a->sent_updates == 1u);
    BOOST_TEST(a->drops == 1u);
    BOOST_TEST(a->max_range_nm.value() == 25.1);
    BOOST_TEST(a->max_range_time_s == kT0 + 70);
    BOOST_TEST(a->max_range_lat == 39.31234567);
    BOOST_TEST(a->max_range_lon == -76.71234567);
    BOOST_TEST(a->max_range_bearing_deg == st.aircraft(icao(0xa4f2c1))->max_range_bearing_deg);
    BOOST_TEST(a->min_alt_ft.value() == 12000);
    BOOST_TEST(a->max_gs_kt.value() == double(412.3f));
    BOOST_TEST(a->min_rssi.value() == -12.3);
    BOOST_TEST((a->callsigns == std::vector<std::string>{"UAL123", "UAL124"}));
    BOOST_TEST((a->squawks == std::vector<std::string>{"1200"}));
    BOOST_TEST((a->inputs == std::vector<std::string>{"readsb", "radar"}));
    BOOST_TEST(a->registration.value() == "N123UA");
    BOOST_TEST(a->category.value() == "A3");
    BOOST_TEST((a->source == skycue::Source::adsb_1090));
    const AircraftStats* b = back.aircraft(skycue::TrackId{0x00abcd, skycue::AddressType::non_icao});
    BOOST_REQUIRE(b);
    BOOST_TEST((b->source == skycue::Source::uat_978));
    BOOST_TEST(b->emergencies == 1u);
    BOOST_TEST(b->min_alt_ft.value() == 0);
    BOOST_TEST(!b->max_gs_kt);
    BOOST_TEST(!b->registration);
    const AircraftStats* c = back.aircraft(icao(0x123456));
    BOOST_REQUIRE(c);
    BOOST_TEST(!c->max_range_nm);
    BOOST_TEST(!c->min_alt_ft);
    BOOST_TEST(!c->min_rssi);

    // Every coverage cell.
    BOOST_TEST(coverageTotal(back) == 3u);
    for (int s = 0; s < kStatsSectors; ++s)
        for (int band = 0; band < kStatsBands; ++band) {
            const CoverageCell& x = st.coverage(s, band);
            const CoverageCell& y = back.coverage(s, band);
            BOOST_TEST(x.max_range_nm == y.max_range_nm);
            BOOST_TEST((x.id == y.id));
            BOOST_TEST(x.time_s == y.time_s);
            BOOST_TEST(x.n == y.n);
        }

    // The series, by_input included.
    for (StatsRes r : {StatsRes::minute, StatsRes::hour}) {
        const auto& x = st.series(r);
        const auto& y = back.series(r);
        BOOST_REQUIRE(x.size() == y.size());
        for (std::size_t i = 0; i < x.size(); ++i) {
            BOOST_TEST(x[i].start_s == y[i].start_s);
            BOOST_TEST(x[i].observations == y[i].observations);
            BOOST_TEST(x[i].uat_downlink == y[i].uat_downlink);
            BOOST_TEST(x[i].tcp_clients == y[i].tcp_clients);
            BOOST_TEST(x[i].drops_filtered == y[i].drops_filtered);
            BOOST_TEST(x[i].max_range_nm == y[i].max_range_nm);
            BOOST_TEST((x[i].max_range_id == y[i].max_range_id));
            BOOST_TEST((x[i].by_input == y[i].by_input));
        }
    }
    BOOST_TEST(back.series(StatsRes::minute).front().by_input.at("readsb") == 1u);
    BOOST_TEST(back.series(StatsRes::hour).front().by_input.at("dump978") == 1u);
    BOOST_TEST(back.series(StatsRes::hour).size() == 3u);

    BOOST_TEST(back.sinceS().value() == kT0 + 5);
    BOOST_TEST(back.seriesSinceS().value() == st.seriesSinceS().value());
    BOOST_TEST(back.uatSeen());
    BOOST_TEST(back.gridCells().size() == st.gridCells().size());
    for (int h = 0; h < 6; ++h)
        BOOST_TEST((back.histogram(StatsHist(h)).counts == st.histogram(StatsHist(h)).counts));
    for (int k = 0; k < 6; ++k)
        BOOST_TEST((back.counts(StatsCount(k)) == st.counts(StatsCount(k))));
    BOOST_TEST(back.counts(StatsCount::squawk_emergency).at("7700") == 1u);
}

BOOST_AUTO_TEST_CASE(round_trip_empty_store)
{
    const StatsStore st(kRx);
    const std::string text = st.toJson();
    const StatsStore back = StatsStore::fromJson(text, kRx, StatsLimits());
    BOOST_TEST(back.toJson() == text);
    BOOST_TEST(!back.sinceS());
    BOOST_TEST(!back.seriesSinceS());
    BOOST_TEST(back.aircraftCount() == 0u);
    BOOST_TEST(back.series(StatsRes::minute).empty());
    BOOST_TEST(!back.uatSeen());
}

BOOST_AUTO_TEST_CASE(file_size_at_defaults_under_10mb)
{
    // 20,000 aircraft, each seen four times (four sessions) with four
    // callsigns, squawks and inputs and every extreme set; 40,000 grid cells;
    // both rings full (30.5 days) with 16 inputs. Full-precision positions
    // and ranges, as the store keeps them.
    StatsStore st(kRx);
    const int kObs = 80000;
    for (int i = 0; i < kObs; ++i) {
        const uint32_t a = uint32_t(i % 20000);
        const int k = i / 20000;
        const int cell = i % 40000;
        const double frac = double((i * 7919) % 1000) / 1000.0;
        const double lat = 30.0 + double(cell / 200) * 0.05 + 0.005 + 0.04 * frac;
        const double lon = -90.0 + double(cell % 200) * 0.05 + 0.005 + 0.04 * (1.0 - frac);
        Obs o = obsAt(0x100000 + a, kT0 + int64_t(i) * 33, lat, lon);
        o.obs.alt_baro_ft = 1000 + (i * 37) % 40000;
        o.obs.gs_kt = 100.0f + float(i % 5000) / 10.0f;
        o.obs.baro_rate_fpm = float((i % 64) * 64 - 2048);
        o.obs.rssi = -3.0 - double(i % 400) / 10.0;
        o.obs.callsign = "CS" + std::to_string(a) + char('A' + k);
        char sq[8];
        std::snprintf(sq, sizeof sq, "%04o", unsigned(a * 4 + uint32_t(k)) % 4096u);
        o.obs.squawk = sq;
        o.obs.registration = "N" + std::to_string(a);
        o.obs.category = "A3";
        o.obs.input_id = "input-" + std::to_string((int(a) + k) % 16);
        sync(o);
        feed(st, o, resultFor(10.0 + std::fmod(double(i) * 0.7316283, 240.0), 1.0 + frac, Decision::send));
    }
    BOOST_REQUIRE(st.aircraftCount() == 20000u);
    BOOST_REQUIRE(st.gridCells().size() == 40000u);
    BOOST_REQUIRE(st.series(StatsRes::minute).size() == kStatsMinuteBuckets);
    BOOST_REQUIRE(st.series(StatsRes::hour).size() == kStatsHourBuckets);
    BOOST_REQUIRE(st.counts(StatsCount::input).size() == 16u);
    BOOST_REQUIRE(st.aircraft(icao(0x100000))->inputs.size() == 4u);
    BOOST_REQUIRE(st.aircraft(icao(0x100000))->callsigns.size() == 4u);

    const auto t0 = std::chrono::steady_clock::now();
    const std::string text = st.toJson();
    const auto t1 = std::chrono::steady_clock::now();
    const StatsStore back = StatsStore::fromJson(text, kRx, StatsLimits());
    const auto t2 = std::chrono::steady_clock::now();
    const auto ms = [](auto d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
    BOOST_TEST_MESSAGE("stats file at the defaults: " << text.size() << " bytes; toJson " << ms(t1 - t0)
                                                      << " ms, fromJson " << ms(t2 - t1) << " ms");
    BOOST_TEST(text.size() < std::size_t(10) * 1024 * 1024);
#ifdef NDEBUG
    const long long load_limit_ms = 5000;
#else
    const long long load_limit_ms = 15000;  // Debug: PropertyTree alone takes ~3 s
#endif
    BOOST_TEST(ms(t2 - t1) < load_limit_ms * testSlowFactor());
    BOOST_TEST(back.aircraftCount() == 20000u);
    BOOST_TEST((back.toJson() == text));
}

BOOST_AUTO_TEST_CASE(malformed_files_rejected)
{
    const std::string base = populatedStore().toJson();
    StatsStore full(kRx);
    for (int64_t m = 0; m < int64_t(kStatsMinuteBuckets); ++m)
        feed(full, obsAt(1, kT0 + m * 60, 39.0, -77.0));
    const std::string full_text = full.toJson();

    // The unedited texts load (so each rejection below is the edit's).
    {
        StatsDir d;
        const std::string path = d.path + "/stats.json";
        for (const std::string* t : {&base, &full_text}) {
            writeText(path, *t);
            StatsStore st(kRx);
            BOOST_REQUIRE(loadStatsFile(path, st) == StatsLoad::loaded);
        }
    }

    auto replaced = [](std::string s, const std::string& from, const std::string& to) {
        const std::size_t p = s.find(from);
        if (p == std::string::npos)
            throw std::runtime_error("no " + from);
        return s.replace(p, from.size(), to);
    };
    std::string oversize = base;
    oversize.append(kStatsFileMaxBytes + 1 - base.size(), ' ');

    const std::vector<std::pair<std::string, std::string>> cases = {
        {"truncated", "{"},
        {"array", "[]"},
        {"empty", ""},
        {"version 2", replaced(base, "\"version\":1", "\"version\":2")},
        {"no version", replaced(base, "\"version\":1,", "")},
        {"missing minutes", replaced(base, "\"minutes\":", "\"minutez\":")},
        {"short column", editFirstElement(base, "\"minutes\":{", "\"observations\":[", "", true)},
        {"1441 minutes", prependMinute(full_text, kT0)},
        {"negative count", editFirstElement(base, "\"minutes\":{", "\"observations\":[", "-1")},
        {"negative evicted", replaced(base, "\"evicted\":0", "\"evicted\":-1")},
        {"hist.range of 49", editFirstElement(base, "\"hist\":{", "\"range\":[", "", true)},
        {"coverage key", editFirstElement(base, "\"coverage\":{", "\"key\":[", "\"zz|icao\"")},
        {"aircraft key", editFirstElement(base, "\"aircraft\":{", "\"key\":[", "\"a4f2c1|nonsense\"")},
        {"non-contiguous", editFirstElement(base, "\"minutes\":{", "\"start\":[", std::to_string(kT0 - 600))},
        {"oversize", oversize},
    };
    for (const auto& c : cases) {
        BOOST_TEST_CONTEXT(c.first)
        {
            StatsDir d;
            const std::string path = d.path + "/stats.json";
            writeText(path + ".bad", "an older bad file");
            writeText(path, c.second);
            StatsStore st = populatedStore();
            const std::string before = st.toJson();
            BOOST_TEST((loadStatsFile(path, st) == StatsLoad::rejected));
            BOOST_TEST(!fileExists(path));
            BOOST_TEST(fileExists(path + ".bad"));
            BOOST_TEST((readText(path + ".bad") == c.second));
            BOOST_TEST((st.toJson() == before));
            BOOST_CHECK_THROW(StatsStore::fromJson(c.second, kRx, StatsLimits()), std::runtime_error);
        }
    }
}

BOOST_AUTO_TEST_CASE(missing_file_is_empty_store)
{
    StatsDir d;
    const std::string path = d.path + "/none.json";
    StatsStore st(kRx);
    BOOST_TEST((loadStatsFile(path, st) == StatsLoad::missing));
    BOOST_TEST(!fileExists(path + ".bad"));
    BOOST_TEST(st.aircraftCount() == 0u);
    BOOST_TEST(!st.sinceS());
}

BOOST_AUTO_TEST_CASE(save_then_load)
{
    StatsDir d;
    const std::string path = d.path + "/stats.json";
    const StatsStore st = populatedStore();
    saveStatsFile(path, StatsStore(kRx));  // replaced by the next save
    saveStatsFile(path, st);
    BOOST_TEST(!fileExists(path + ".tmp"));
    BOOST_TEST(readText(path) == st.toJson());
    StatsStore fresh(kRx);
    BOOST_TEST((loadStatsFile(path, fresh) == StatsLoad::loaded));
    BOOST_TEST(fresh.toJson() == st.toJson());
    BOOST_TEST(!fresh.dirty());
    BOOST_TEST(fileExists(path));
    BOOST_TEST(!fileExists(path + ".bad"));
    BOOST_CHECK_THROW(saveStatsFile(d.path + "/no-such-dir/stats.json", st), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(load_evicts_down_to_cap)
{
    StatsStore big(kRx);
    for (uint32_t a = 0; a < 120; ++a)
        feed(big, obsAt(a + 1, kT0 + int64_t(a), 39.0, -77.0));
    BOOST_REQUIRE(big.aircraftCount() == 120u);
    StatsDir d;
    const std::string path = d.path + "/stats.json";
    saveStatsFile(path, big);
    StatsLimits lim;
    lim.aircraft_max = 100;
    StatsStore small(kRx, lim);
    BOOST_TEST((loadStatsFile(path, small) == StatsLoad::loaded));
    BOOST_TEST(small.aircraftCount() <= 100u);
    BOOST_TEST(small.evicted() == 120u - small.aircraftCount());
    BOOST_TEST(small.aircraft(icao(120)));   // the newest stays
    BOOST_TEST(!small.aircraft(icao(1)));    // the oldest goes
    BOOST_TEST(small.limits().aircraft_max == 100u);
}

BOOST_AUTO_TEST_CASE(restart_on_first_older_obs_after_load)
{
    StatsStore st(kRx);
    const int64_t t = kT0 + 3 * 3600 + 120;
    feed(st, obsAt(1, t - 600, 39.0, -77.0));
    feed(st, obsAt(1, t, 39.0, -77.0));
    StatsStore back = StatsStore::fromJson(st.toJson(), kRx, StatsLimits());
    BOOST_REQUIRE(back.series(StatsRes::minute).back().start_s == t);
    feed(back, obsAt(2, t - 3600, 39.0, -77.0));
    BOOST_TEST(back.series(StatsRes::minute).size() == 1u);
    BOOST_TEST(back.series(StatsRes::minute).front().start_s == t - 3600);
    BOOST_TEST(back.series(StatsRes::hour).size() == 1u);
    BOOST_TEST(back.seriesSinceS().value() == t - 3600);
    BOOST_TEST(back.aircraftCount() == 2u);
    BOOST_TEST(back.aircraft(icao(1))->observations == 2u);
}

BOOST_AUTO_TEST_CASE(counts_capped_at_int64_max_on_load)
{
    // The file's integers are written as int64: a larger count is capped on
    // load, so the next save and load still work.
    const std::string base = populatedStore().toJson();
    const std::size_t p = base.find("\"evicted\":0");
    BOOST_REQUIRE(p != std::string::npos);
    std::string text = base;
    text.replace(p, 11, "\"evicted\":18446744073709551615");
    text = editFirstElement(text, "\"minutes\":{", "\"observations\":[", "18446744073709551615");
    const StatsStore st = StatsStore::fromJson(text, kRx, StatsLimits());
    const uint64_t cap = uint64_t(std::numeric_limits<int64_t>::max());
    BOOST_TEST(st.evicted() == cap);
    BOOST_TEST(st.series(StatsRes::minute).front().observations == cap);
    const StatsStore again = StatsStore::fromJson(st.toJson(), kRx, StatsLimits());
    BOOST_TEST(again.evicted() == cap);
    BOOST_TEST(again.toJson() == st.toJson());
    // Over 2^64 - 1 is not a count at all.
    BOOST_CHECK_THROW(StatsStore::fromJson(editFirstElement(base, "\"minutes\":{", "\"observations\":[",
                                                            "18446744073709551616"),
                                           kRx, StatsLimits()),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(whitespace_in_list_entries)
{
    StatsStore st(kRx);
    Obs o = obsAt(1, kT0, 39.0, -77.0);
    o.obs.callsign = "AB\tC D";
    sync(o);
    feed(st, o);
    o.obs.received_time = at(kT0 + 1);
    o.obs.callsign = "XYZ";
    sync(o);
    feed(st, o);
    const StatsStore back = StatsStore::fromJson(st.toJson(), kRx, StatsLimits());
    BOOST_TEST((back.aircraft(icao(1))->callsigns == std::vector<std::string>{"AB_C_D", "XYZ"}));
    BOOST_TEST(back.toJson() == StatsStore::fromJson(back.toJson(), kRx, StatsLimits()).toJson());
}

BOOST_AUTO_TEST_CASE(missing_evicted_rejected)
{
    std::string text = populatedStore().toJson();
    const std::size_t p = text.find("\"evicted\":0,");
    BOOST_REQUIRE(p != std::string::npos);
    text.erase(p, 12);
    try {
        StatsStore::fromJson(text, kRx, StatsLimits());
        BOOST_ERROR("no exception");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()) == "stats file: evicted: missing");
    }
}
