#define BOOST_TEST_MODULE skycued_stats_store
#include <boost/test/unit_test.hpp>

#include "engine_events.hpp"
#include "stats_store.hpp"
#include "test_slow.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>

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
    BOOST_TEST(one.seriesSinceS().value() == b);
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

BOOST_AUTO_TEST_CASE(tick_deltas_and_samples)
{
    StatsStore st(kRx);
    EngineStats s;
    // Nothing stored and no series: the first tick only takes the base.
    s.uat_downlink = 10;
    s.tcp_clients = 2;
    st.tick(at(kT0), s);
    BOOST_TEST(st.series(StatsRes::minute).empty());
    BOOST_TEST(!st.uatSeen());
    BOOST_TEST(!st.dirty());
    s.uat_downlink = 15;
    st.tick(at(kT0 + 1), s);
    BOOST_REQUIRE(st.series(StatsRes::minute).size() == 1u);
    BOOST_TEST(st.series(StatsRes::minute).back().uat_downlink == 5u);
    BOOST_TEST(st.series(StatsRes::minute).back().tcp_clients == 2u);
    BOOST_TEST(st.uatSeen());
    BOOST_TEST(st.dirty());
    s.uat_downlink = 3;  // the counter went down: a new base
    st.tick(at(kT0 + 2), s);
    BOOST_TEST(st.series(StatsRes::minute).back().uat_downlink == 5u);
    s.uat_downlink = 4;
    s.uat_uplink = 0;
    s.fisb_products = 0;
    st.tick(at(kT0 + 3), s);
    BOOST_TEST(st.series(StatsRes::minute).back().uat_downlink == 6u);

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
