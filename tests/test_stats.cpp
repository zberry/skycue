#define BOOST_TEST_MODULE skycued_stats
#include <boost/test/unit_test.hpp>

#include "geo.hpp"
#include "recording_reader.hpp"
#include "stats_report.hpp"
#include "time_util.hpp"

#include <chrono>
#include <string>
#include <vector>

using namespace skycued;
using skycue::AddressType;
using skycue::Source;

namespace {

// 2026-11-01T15:00:00Z, on a whole second.
const std::chrono::system_clock::time_point kT0{std::chrono::seconds(1793545200)};

std::chrono::system_clock::time_point at(double seconds)
{
    using std::chrono::system_clock;
    return kT0 + std::chrono::duration_cast<system_clock::duration>(std::chrono::duration<double>(seconds));
}

RecordingRecord obs(double t, uint32_t address)
{
    RecordingRecord r;
    r.time = at(t);
    r.kind = RecordKind::obs;
    r.obs.id.address = address;
    r.obs.source = Source::adsb_1090;
    r.obs.received_time = r.time;
    return r;
}

RecordingRecord obsAt(double t, uint32_t address, double lat, double lon, int32_t alt_ft)
{
    RecordingRecord r = obs(t, address);
    r.obs.lat = lat;
    r.obs.lon = lon;
    r.obs.position_time = r.time;
    r.obs.alt_baro_ft = alt_ft;
    return r;
}

RecordingRecord sent(double t, RecordKind kind, uint32_t address)
{
    RecordingRecord r;
    r.time = at(t);
    r.kind = kind;
    r.obs.id.address = address;
    return r;
}

StatsOptions withReceiver()
{
    StatsOptions o;
    o.rx_lat = 38.9;
    o.rx_lon = -77.0;
    o.receiver_from = "--lat/--lon";
    return o;
}

const char* const kSkipNote = "(skipped: receiver position unknown; pass --lat/--lon or --config)";

std::size_t count(const std::string& haystack, const std::string& needle)
{
    std::size_t n = 0;
    for (auto p = haystack.find(needle); p != std::string::npos; p = haystack.find(needle, p + needle.size()))
        ++n;
    return n;
}

}  // namespace

BOOST_AUTO_TEST_CASE(overview_counts_and_duration)
{
    std::vector<RecordingRecord> recs{obs(10, 0xa1), obs(0, 0xa1), obs(0.5, 0xa2)};
    RecordingRecord up;
    up.time = at(1);
    up.kind = RecordKind::uat_uplink;
    recs.push_back(up);

    const StatsReport r = computeStats(recs, StatsOptions{});
    BOOST_TEST(r.overview.records == 4u);
    BOOST_TEST(r.overview.by_kind.at("obs") == 3u);
    BOOST_TEST(r.overview.by_kind.at("uat_uplink") == 1u);
    BOOST_REQUIRE(r.overview.first.has_value());
    BOOST_TEST(formatIso8601Ms(*r.overview.first) == "2026-11-01T15:00:00.000Z");
    BOOST_TEST(formatIso8601Ms(*r.overview.last) == "2026-11-01T15:00:10.000Z");
    BOOST_TEST(r.overview.duration_s == 10.0, boost::test_tools::tolerance(1e-9));
    BOOST_TEST(r.overview.mean_per_s == 0.4, boost::test_tools::tolerance(1e-9));
    BOOST_TEST(r.overview.peak_per_s == 2u);  // 0.0 and 0.5 share one second
    BOOST_TEST(r.aircraft.distinct == 2u);
    BOOST_TEST(r.aircraft.observations == 3u);

    const std::string text = formatStatsText(r);
    BOOST_TEST(text.find("== Overview ==") != std::string::npos);
    BOOST_TEST(text.find("Aircraft: 2\n") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(range_histogram_bins_and_max)
{
    // Due north of the receiver at about 5, 25 and 55 nm (1 nm ~ 1/60 deg).
    std::vector<RecordingRecord> recs{
        obsAt(0, 0xabc001, 38.9 + 5.0 / 60, -77.0, 10000),
        obsAt(1, 0xabc002, 38.9 + 25.0 / 60, -77.0, 10000),
        obsAt(2, 0xabc003, 38.9 + 55.0 / 60, -77.0, 10000),
        obsAt(3, 0xabc002, 38.9 + 26.0 / 60, -77.0, 10000),
        obs(4, 0xabc004),  // no position: not in the histogram
    };
    const StatsReport r = computeStats(recs, withReceiver());
    BOOST_REQUIRE(r.range.computed);
    BOOST_TEST(r.range.positions == 4u);
    BOOST_REQUIRE(r.range.bins.size() == 6u);  // 0-10 ... 50-60
    BOOST_TEST(r.range.bins[0].obs == 1u);
    BOOST_TEST(r.range.bins[1].obs == 0u);
    BOOST_TEST(r.range.bins[2].obs == 2u);
    BOOST_TEST(r.range.bins[2].aircraft == 1u);
    BOOST_TEST(r.range.bins[5].obs == 1u);
    BOOST_TEST(r.range.bins[5].start == 50.0);
    BOOST_TEST(r.range.bins[5].end == 60.0);
    BOOST_REQUIRE(r.range.max.has_value());
    BOOST_TEST(r.range.max->id.address == 0xabc003u);
    BOOST_TEST(r.range.max->value > 54.5);
    BOOST_TEST(r.range.max->value < 55.5);
    BOOST_TEST(formatIso8601Ms(r.range.max->time) == "2026-11-01T15:00:02.000Z");
    // Median of ~5, ~25, ~26, ~55: (25 + 26) / 2.
    BOOST_TEST(r.range.median_nm > 25.0);
    BOOST_TEST(r.range.median_nm < 26.0);

    const std::string text = formatStatsText(r);
    BOOST_TEST(text.find("== Range histogram (10 nm bins) ==") != std::string::npos);
    BOOST_TEST(text.find("abc003") != std::string::npos);
    // The largest bar is 40 characters.
    BOOST_TEST(text.find(std::string(40, '#')) != std::string::npos);
    BOOST_TEST(text.find(std::string(41, '#')) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(azimuth_sectors)
{
    // Due north; and east along the great circle (the parallel itself curves
    // north of it, giving 89.x deg, so sit a hair south of it).
    std::vector<RecordingRecord> recs{
        obsAt(0, 0xa1, 38.9 + 10.0 / 60, -77.0, 5000),
        obsAt(1, 0xa2, 38.89, -76.8, 5000),
        obsAt(2, 0xa2, 38.89, -76.7, 5000),
    };
    const StatsReport r = computeStats(recs, withReceiver());
    BOOST_REQUIRE(r.azimuth.computed);
    BOOST_REQUIRE(r.azimuth.sectors.size() == 12u);
    BOOST_TEST(r.azimuth.sectors[0].obs == 1u);
    BOOST_TEST(r.azimuth.sectors[0].aircraft == 1u);
    BOOST_TEST(r.azimuth.sectors[3].obs == 2u);
    BOOST_TEST(r.azimuth.sectors[3].aircraft == 1u);
    BOOST_TEST(r.azimuth.sectors[3].start == 90.0);
    BOOST_TEST(r.azimuth.sectors[3].max_range_nm.value_or(0) ==
                   geo::greatCircleNm(38.9, -77.0, 38.89, -76.7), boost::test_tools::tolerance(1e-9));
    for (int s : {1, 2, 4, 5, 6, 7, 8, 9, 10, 11})
        BOOST_TEST(r.azimuth.sectors[s].obs == 0u);

    const std::string text = formatStatsText(r);
    BOOST_TEST(text.find("== Coverage by azimuth (30 deg sectors) ==") != std::string::npos);
    BOOST_TEST(text.find("  330-360") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(elevation_histogram)
{
    std::vector<RecordingRecord> recs{
        obsAt(0, 0xa1, 38.9 + 10.0 / 60, -77.0, 10000),   // ~9.3 deg
        obsAt(1, 0xa2, 38.9 + 1.0 / 60, -77.0, 30000),    // ~78.5 deg
        obsAt(2, 0xa3, 38.9 + 100.0 / 60, -77.0, 1000),   // below the horizon
    };
    const StatsReport r = computeStats(recs, withReceiver());
    BOOST_REQUIRE(r.elevation.computed);
    BOOST_REQUIRE(r.elevation.bins.size() == 18u);
    BOOST_TEST(r.elevation.bins[1].obs == 1u);
    BOOST_TEST(r.elevation.bins[1].start == 5.0);
    BOOST_TEST(r.elevation.bins[15].obs == 1u);
    BOOST_TEST(r.elevation.below_zero == 1u);
    BOOST_TEST(r.elevation.samples == 3u);

    const std::string text = formatStatsText(r);
    BOOST_TEST(text.find("== Elevation histogram (5 deg bins) ==") != std::string::npos);
    BOOST_TEST(text.find("Below 0 deg: 1") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(gap_detection)
{
    std::vector<RecordingRecord> recs{obs(0, 0xa1), obs(12, 0xa1), obs(14, 0xa1)};
    StatsOptions o;
    o.gap_s = 5;
    const StatsReport r = computeStats(recs, o);
    BOOST_REQUIRE(r.gaps.size() == 1u);
    BOOST_TEST(r.gaps[0].length_s == 12.0, boost::test_tools::tolerance(1e-9));
    BOOST_TEST(formatIso8601Ms(r.gaps[0].start) == "2026-11-01T15:00:00.000Z");
    BOOST_TEST(formatIso8601Ms(r.gaps[0].end) == "2026-11-01T15:00:12.000Z");

    const std::string text = formatStatsText(r);
    BOOST_TEST(text.find("== Gaps (> 5 s without records) ==") != std::string::npos);
    BOOST_TEST(text.find("12.0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(sent_stream_drop_reasons_and_compression)
{
    std::vector<RecordingRecord> recs{
        obs(0, 0xa1), obs(1, 0xa1), obs(2, 0xa1), obs(0, 0xb2), obs(1, 0xb2), obs(0, 0xc3),
        sent(0, RecordKind::update, 0xa1), sent(1, RecordKind::update, 0xa1), sent(0, RecordKind::update, 0xb2),
    };
    RecordingRecord d = sent(5, RecordKind::drop, 0xa1);
    d.drop_reason = "timeout";
    recs.push_back(d);
    d = sent(6, RecordKind::drop, 0xb2);
    d.drop_reason = "filtered";
    recs.push_back(d);
    d.time = at(7);
    recs.push_back(d);
    RecordingRecord f = sent(0, RecordKind::filter_change, 0);
    f.note = "range<=150nm";
    recs.push_back(f);

    const StatsReport r = computeStats(recs, StatsOptions{});
    BOOST_REQUIRE(r.sent.present);
    BOOST_TEST(r.basis == "received");
    BOOST_TEST(r.aircraft.distinct == 3u);  // from the received stream, not doubled
    BOOST_TEST(r.sent.updates == 3u);
    BOOST_TEST(r.sent.keyframes == 0u);
    BOOST_TEST(r.sent.drops.at("timeout") == 1u);
    BOOST_TEST(r.sent.drops.at("filtered") == 2u);
    BOOST_TEST(r.sent.drops.at("shutdown") == 0u);
    BOOST_REQUIRE(r.sent.filter_changes.size() == 1u);
    BOOST_TEST(r.sent.filter_changes[0].second == "range<=150nm");
    BOOST_REQUIRE(r.sent.updates_per_aircraft.size() == 2u);
    BOOST_TEST(r.sent.updates_per_aircraft[0].first.address == 0xa1u);
    BOOST_TEST(r.sent.updates_per_aircraft[0].second == 2u);
    BOOST_REQUIRE(r.sent.compared);
    BOOST_TEST(r.sent.received_obs == 6u);
    BOOST_TEST(r.sent.compression == 2.0, boost::test_tools::tolerance(1e-9));
    BOOST_REQUIRE(r.sent.never_sent.size() == 1u);
    BOOST_TEST(r.sent.never_sent[0].address == 0xc3u);

    const std::string text = formatStatsText(r);
    BOOST_TEST(text.find("== Sent stream ==") != std::string::npos);
    BOOST_TEST(text.find("Drops by reason") != std::string::npos);
    BOOST_TEST(text.find("Keyframe events: 0") != std::string::npos);
    BOOST_TEST(text.find("Compression: 6 received obs / 3 updates = 2.00") != std::string::npos);
    BOOST_TEST(text.find("Received aircraft never sent: 1") != std::string::npos);
    const std::string json = formatStatsJson(r);
    BOOST_TEST(json.find("\"compression\":2") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(sent_only_uses_updates_as_observations)
{
    std::vector<RecordingRecord> recs{sent(0, RecordKind::update, 0xa1), sent(1, RecordKind::update, 0xb2)};
    const StatsReport r = computeStats(recs, StatsOptions{});
    BOOST_TEST(r.basis == "sent");
    BOOST_TEST(r.aircraft.distinct == 2u);
    BOOST_TEST(!r.sent.compared);
}

BOOST_AUTO_TEST_CASE(squawk_7700_listed)
{
    std::vector<RecordingRecord> recs{obs(0, 0xa1), obs(1, 0xa1), obs(2, 0xa1), obs(3, 0xb2)};
    recs[0].obs.squawk = "1200";
    recs[1].obs.squawk = "7700";
    recs[2].obs.squawk = "7700";
    recs[3].obs.squawk = "7600";
    const StatsReport r = computeStats(recs, StatsOptions{});
    BOOST_REQUIRE(r.aircraft.emergency_squawks.size() == 2u);
    BOOST_TEST(r.aircraft.emergency_squawks[0].id.address == 0xa1u);
    BOOST_TEST(r.aircraft.emergency_squawks[0].squawk == "7700");
    BOOST_TEST(r.aircraft.emergency_squawks[0].obs == 2u);
    BOOST_TEST(formatIso8601Ms(r.aircraft.emergency_squawks[0].first) == "2026-11-01T15:00:01.000Z");
    BOOST_TEST(r.aircraft.emergency_squawks[1].squawk == "7600");

    const std::string text = formatStatsText(r);
    BOOST_TEST(text.find("7700") != std::string::npos);
    BOOST_TEST(text.find("7600") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(skips_range_sections_without_receiver)
{
    std::vector<RecordingRecord> recs{obsAt(0, 0xa1, 39.0, -77.0, 12000)};
    const StatsReport r = computeStats(recs, StatsOptions{});
    BOOST_TEST(!r.range.computed);
    BOOST_TEST(!r.elevation.computed);
    BOOST_TEST(!r.azimuth.computed);
    BOOST_REQUIRE(r.altitude.bins.size() == 1u);  // altitude needs no receiver
    BOOST_TEST(r.altitude.bins[0].start == 10000.0);

    const std::string text = formatStatsText(r);
    BOOST_TEST(count(text, kSkipNote) == 3u);
    const std::string json = formatStatsJson(r);
    BOOST_TEST(json.find("\"range_histogram\":null") != std::string::npos);
    BOOST_TEST(json.find("\"elevation_histogram\":null") != std::string::npos);
    BOOST_TEST(json.find("\"azimuth\":null") != std::string::npos);
    BOOST_TEST(json.find("\"altitude_histogram\":{") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(output_is_deterministic)
{
    std::vector<RecordingRecord> recs{obsAt(0, 0xa1, 39.0, -77.0, 12000), obsAt(0, 0xb2, 39.1, -77.1, 2000),
                                      obsAt(1, 0xa1, 39.0, -77.0, 12100)};
    const StatsReport a = computeStats(recs, withReceiver());
    const StatsReport b = computeStats(recs, withReceiver());
    BOOST_TEST(formatStatsText(a) == formatStatsText(b));
    BOOST_TEST(formatStatsJson(a) == formatStatsJson(b));
}

BOOST_AUTO_TEST_CASE(reads_every_line_kind)
{
    RecordingRecord r;
    BOOST_TEST((parseRecordingLine("   ", r) == LineKind::ignore));
    BOOST_TEST((parseRecordingLine("not json", r) == LineKind::bad));

    BOOST_TEST((parseRecordingLine(R"({"time":"2026-11-01T15:00:00.052Z","type":"obs","hex":"ac5e81",)"
                                  R"("source":"adsb_1090","squawk":"7700"})",
                                  r) == LineKind::received));
    BOOST_TEST((r.kind == RecordKind::obs));
    BOOST_TEST(r.obs.id.address == 0xac5e81u);
    BOOST_TEST(r.obs.squawk.value_or("") == "7700");

    BOOST_TEST((parseRecordingLine(R"({"time":"2026-11-01T15:00:01Z","type":"uat_uplink","uplink_hex":"00"})", r) ==
               LineKind::received));
    BOOST_TEST((r.kind == RecordKind::uat_uplink));

    BOOST_TEST((parseRecordingLine(R"({"now":1793545200.052,"hex":"~2c4a1f","type":"tisb_other"})", r) ==
               LineKind::readsb_raw));
    BOOST_TEST((r.obs.id.type == AddressType::non_icao));
    BOOST_TEST(formatIso8601Ms(r.time) == "2026-11-01T15:00:00.052Z");

    BOOST_TEST((parseRecordingLine(R"({"time":"2026-11-01T15:00:02.000Z","event":"update","hex":"a1b2c3",)"
                                  R"("addr_type":"icao","source":"uat_978","pos_time":"2026-11-01T15:00:01.500Z",)"
                                  R"("lat":38.5,"lon":-77.5,"on_ground":false,"alt_is_geom":false,)"
                                  R"("alt_baro_ft":12000,"gs_kt":250.5,"vrate_fpm":-640,"callsign":"N1",)"
                                  R"("keyframe":true})",
                                  r) == LineKind::sent));
    BOOST_TEST((r.kind == RecordKind::update));
    BOOST_TEST((r.obs.source == Source::uat_978));
    BOOST_TEST(r.obs.lat.value_or(0) == 38.5);
    BOOST_TEST(r.obs.alt_baro_ft.value_or(0) == 12000);
    BOOST_TEST(r.vrate_fpm.value_or(0) == -640.0f);
    BOOST_TEST(r.keyframe_flag);

    BOOST_TEST((parseRecordingLine(R"({"time":"2026-11-01T15:00:03.000Z","event":"drop","hex":"a1b2c3",)"
                                  R"("addr_type":"icao","drop_reason":"source_lost"})",
                                  r) == LineKind::sent));
    BOOST_TEST((r.kind == RecordKind::drop));
    BOOST_TEST(r.drop_reason == "source_lost");
    BOOST_TEST((parseRecordingLine(R"({"time":"2026-11-01T15:00:03.000Z","event":"drop","hex":"a1b2c3",)"
                                  R"("drop_reason":"bogus"})",
                                  r) == LineKind::bad));

    BOOST_TEST((parseRecordingLine(R"({"time":"2026-11-01T15:00:00.000Z","event":"filter_change","note":"x"})", r) ==
               LineKind::sent));
    BOOST_TEST((r.kind == RecordKind::filter_change));
    BOOST_TEST(r.note == "x");
    BOOST_TEST((parseRecordingLine(R"({"time":"2026-11-01T15:00:00.000Z","event":"nope"})", r) == LineKind::bad));
}
