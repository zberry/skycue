#define BOOST_TEST_MODULE skycued_pingstation
#include <boost/test/unit_test.hpp>

#include "asterix/cat021.hpp"
#include "asterix/cat021_encode.hpp"
#include "inputs/cat021_map.hpp"
#include "inputs/device_time.hpp"
#include "inputs/input_types.hpp"
#include "inputs/ping_json.hpp"
#include "json_lite.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace skycued;
using skycue::AddressType;
using skycue::Source;
using std::chrono::system_clock;

namespace {

using TP = system_clock::time_point;

// A UTC time from its calendar fields (timegm, so the test machine's time zone
// does not matter).
TP utc(int y, int mo, int d, int h, int mi, int s, int64_t ns = 0)
{
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    const auto secs = static_cast<int64_t>(timegm(&tm));
    return TP(std::chrono::duration_cast<system_clock::duration>(std::chrono::seconds(secs) +
                                                                  std::chrono::nanoseconds(ns)));
}

int64_t epochNs(TP t)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

int64_t epochMs(TP t)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

double secondsBetween(TP a, TP b) { return std::chrono::duration<double>(a - b).count(); }

std::string fixture(const std::string& name)
{
    std::ifstream in(std::string(SKYCUE_FIXTURE_DIR) + "/pingstation/" + name, std::ios::binary);
    BOOST_REQUIRE_MESSAGE(in, "cannot open fixture " << name);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string replaced(std::string text, const std::string& from, const std::string& to)
{
    const auto pos = text.find(from);
    BOOST_REQUIRE_MESSAGE(pos != std::string::npos, "fixture lacks " << from);
    return text.replace(pos, from.size(), to);
}

PingResult parse(const std::string& text, TimeMode mode = TimeMode::utc)
{
    return parsePingDatagram(text, PingContext{utc(2026, 11, 2, 12, 0, 0), mode});
}

// One bare aircraft object with the given extra members.
TrackObservation one(const std::string& members)
{
    const auto r = parse(R"({"icaoAddress":"ABCDEF")" + (members.empty() ? "" : "," + members) + "}");
    BOOST_REQUIRE_EQUAL(r.observations.size(), 1u);
    BOOST_REQUIRE_EQUAL(r.rejected, 0u);
    return r.observations[0];
}

const TP kIcdArrival = utc(2022, 12, 13, 21, 11, 34);

}  // namespace

// ---- device time ----

BOOST_AUTO_TEST_CASE(iso_fraction_forms)
{
    const int64_t base = epochMs(utc(2022, 12, 13, 21, 11, 32));
    auto ms = [](const char* s) -> std::optional<int64_t> {
        auto t = parseDeviceTime(s);
        if (!t)
            return std::nullopt;
        return epochMs(*t);
    };
    BOOST_TEST(ms("2022-12-13T21:11:32Z").value_or(0) == base);
    BOOST_TEST(ms("2022-12-13T21:11:32.1Z").value_or(0) == base + 100);
    BOOST_TEST(ms("2022-12-13T21:11:32:317283895Z").value_or(0) == base + 317);
    BOOST_TEST(ms("2020-11-23T23:02:22.3725496Z").value_or(0) == epochMs(utc(2020, 11, 23, 23, 2, 22)) + 372);
    BOOST_TEST(epochNs(parseDeviceTime("2022-12-13T21:11:32.317283895Z").value()) ==
               epochNs(utc(2022, 12, 13, 21, 11, 32, 317283895)));
    BOOST_TEST(ms("2024-02-29T00:00:00Z").value_or(0) == epochMs(utc(2024, 2, 29, 0, 0, 0)));

    BOOST_TEST(!parseDeviceTime("2022-13-01T00:00:00Z"));
    BOOST_TEST(!parseDeviceTime("2023-02-29T00:00:00Z"));
    BOOST_TEST(!parseDeviceTime("2022-12-13T24:00:00Z"));
    BOOST_TEST(!parseDeviceTime("2022-12-13T21:60:00Z"));
    BOOST_TEST(!parseDeviceTime("2022-12-13T21:11:32"));        // no Z
    BOOST_TEST(!parseDeviceTime("2022-12-13T21:11:32.Z"));      // no fraction digits
    BOOST_TEST(!parseDeviceTime("2022-12-13T21:11:32.1234567890Z"));  // 10 digits
    BOOST_TEST(!parseDeviceTime("2022-12-13 21:11:32Z"));
    BOOST_TEST(!parseDeviceTime("2022-12-13T21:11:32Zx"));
    BOOST_TEST(!parseDeviceTime(""));
}

BOOST_AUTO_TEST_CASE(clock_check_window)
{
    const TP arrival = utc(2026, 11, 2, 12, 0, 0);
    auto none = checkDeviceTime(std::nullopt, arrival, TimeMode::utc);
    BOOST_TEST((none.used == arrival));
    BOOST_TEST(!none.offset_s);
    BOOST_TEST(!none.rejected);

    auto edge = checkDeviceTime(arrival + std::chrono::seconds(10), arrival, TimeMode::utc);
    BOOST_TEST((edge.used == arrival + std::chrono::seconds(10)));
    BOOST_TEST(edge.offset_s.value_or(0) == 10.0);
    BOOST_TEST(!edge.rejected);

    auto late = checkDeviceTime(arrival - std::chrono::milliseconds(10001), arrival, TimeMode::utc);
    BOOST_TEST((late.used == arrival));
    BOOST_TEST(late.offset_s.value_or(0) == -10.001, boost::test_tools::tolerance(1e-9));
    BOOST_TEST(late.rejected);

    auto gps = checkDeviceTime(arrival + std::chrono::seconds(20), arrival, TimeMode::gps);
    BOOST_TEST((gps.used == arrival + std::chrono::seconds(2)));
    BOOST_TEST(gps.offset_s.value_or(0) == 2.0);
    BOOST_TEST(!gps.rejected);
}

BOOST_AUTO_TEST_CASE(tod_midnight_wrap)
{
    auto near = [](TP a, TP b) { return std::abs(secondsBetween(a, b)) < 1e-6; };
    BOOST_TEST(near(timeOfDay(0.2, utc(2026, 11, 2, 23, 59, 59, 900000000)),
                    utc(2026, 11, 3, 0, 0, 0, 200000000)));
    BOOST_TEST(near(timeOfDay(86399.8, utc(2026, 11, 3, 0, 0, 0, 100000000)),
                    utc(2026, 11, 2, 23, 59, 59, 800000000)));
    BOOST_TEST(near(timeOfDay(11 * 3600.0, utc(2026, 11, 2, 12, 0, 0)), utc(2026, 11, 2, 11, 0, 0)));
    BOOST_TEST(near(timeOfDay(13 * 3600.0, utc(2026, 11, 2, 12, 0, 0)), utc(2026, 11, 2, 13, 0, 0)));
}

BOOST_AUTO_TEST_CASE(gps_counter_time)
{
    BOOST_TEST(epochNs(gpsCounterTime(0, 0)) == int64_t{315964800} * 1000000000);
    BOOST_TEST(epochNs(gpsCounterTime(1355001092, 317283895)) == int64_t{1670965892317283895});
}

// ---- JSON ----

BOOST_AUTO_TEST_CASE(icd_1090_example_maps)
{
    const std::string text = fixture("icd-traffic-1090.json");
    const auto r = parsePingDatagram(text, PingContext{kIcdArrival, TimeMode::utc});
    BOOST_TEST(r.rejected == 0u);
    BOOST_TEST(r.clock_rejects == 0u);
    BOOST_TEST(!r.status);
    BOOST_REQUIRE_EQUAL(r.observations.size(), 1u);
    const auto& o = r.observations[0];
    BOOST_TEST(o.id.address == 0xC00586u);
    BOOST_TEST((o.id.type == AddressType::icao));
    BOOST_TEST((o.source == Source::adsb_1090));
    BOOST_TEST((o.received_time == kIcdArrival));
    BOOST_TEST(o.lat.value_or(0) == 42.132614);
    BOOST_TEST(o.lon.value_or(0) == -93.007891);
    BOOST_TEST(o.alt_baro_ft.value_or(0) == 34025);
    BOOST_TEST(!o.alt_geom_ft);  // baroaltDiffMM is not used
    BOOST_TEST(o.track_deg.value_or(0) == 263.3f, boost::test_tools::tolerance(0.001f / 263.3f));
    BOOST_TEST(o.gs_kt.value_or(0) == 410.99f, boost::test_tools::tolerance(0.01f / 410.99f));
    BOOST_TEST(o.geom_rate_fpm.value_or(-1) == 0.0f);  // verVelocitySrc 1
    BOOST_TEST(!o.baro_rate_fpm);
    BOOST_TEST(o.squawk.value_or("") == "1305");
    BOOST_TEST(o.callsign.value_or("") == "CJT340");
    BOOST_TEST(o.category.value_or("") == "A5");
    BOOST_TEST(o.emergency.value_or("") == "none");
    BOOST_TEST((o.on_ground == std::optional<bool>(false)));
    BOOST_TEST(o.nic.value_or(0) == 8);
    BOOST_TEST(o.nac_p.value_or(0) == 9);
    BOOST_TEST(!o.rssi);
    BOOST_TEST(!o.registration);
    BOOST_REQUIRE(o.position_time);
    BOOST_TEST((*o.position_time == utc(2022, 12, 13, 21, 11, 32)));
    BOOST_TEST(o.raw_line == text);
    BOOST_TEST(r.clock_offset_s.value_or(0) == -2.0, boost::test_tools::tolerance(1e-9));

    BOOST_REQUIRE_EQUAL(r.raw_frames.size(), 1u);
    BOOST_TEST((r.raw_frames[0].kind == RawFrame::Kind::adsb_1090));
    BOOST_TEST(r.raw_frames[0].hex == "8dc0058658af979ecbc80d0c3c70");
    BOOST_TEST(epochNs(r.raw_frames[0].time) == int64_t{1670965892317283895});
}

BOOST_AUTO_TEST_CASE(icd_1090_example_clamped_when_far)
{
    const TP arrival = utc(2026, 1, 1, 0, 0, 0);
    const auto r = parsePingDatagram(fixture("icd-traffic-1090.json"), PingContext{arrival, TimeMode::utc});
    BOOST_REQUIRE_EQUAL(r.observations.size(), 1u);
    const auto& o = r.observations[0];
    BOOST_REQUIRE(o.position_time);
    BOOST_TEST((*o.position_time == arrival));
    BOOST_TEST((o.received_time == arrival));
    BOOST_TEST(r.clock_rejects == 1u);
    BOOST_TEST(r.clock_offset_s.value_or(0) < -9e7);  // about three years behind
    BOOST_REQUIRE_EQUAL(r.raw_frames.size(), 1u);
    BOOST_TEST((r.raw_frames[0].time == arrival));
}

BOOST_AUTO_TEST_CASE(gps_time_mode_subtracts_18s)
{
    const TP arrival = utc(2022, 12, 13, 21, 11, 14);
    const auto r = parsePingDatagram(fixture("icd-traffic-1090.json"), PingContext{arrival, TimeMode::gps});
    BOOST_REQUIRE_EQUAL(r.observations.size(), 1u);
    BOOST_TEST((r.observations[0].position_time.value() == arrival));
    BOOST_TEST(r.clock_offset_s.value_or(1) == 0.0);
    BOOST_TEST(r.clock_rejects == 0u);
}

BOOST_AUTO_TEST_CASE(utcsync_zero_uses_arrival)
{
    const auto text = replaced(fixture("icd-traffic-1090.json"), R"("utcSync":1)", R"("utcSync":0)");
    const auto r = parsePingDatagram(text, PingContext{kIcdArrival, TimeMode::utc});
    BOOST_REQUIRE_EQUAL(r.observations.size(), 1u);
    BOOST_TEST((r.observations[0].position_time.value() == kIcdArrival));
    BOOST_TEST(!r.clock_offset_s);
    BOOST_TEST(r.clock_rejects == 0u);
    BOOST_REQUIRE_EQUAL(r.raw_frames.size(), 1u);
    BOOST_TEST((r.raw_frames[0].time == kIcdArrival));
}

BOOST_AUTO_TEST_CASE(icd_status_verbatim_is_rejected)
{
    const std::string text = fixture("icd-status-verbatim.json");
    PingResult r;
    BOOST_CHECK_NO_THROW(r = parsePingDatagram(text, PingContext{kIcdArrival, TimeMode::utc}));
    BOOST_TEST(r.rejected == 1u);
    BOOST_TEST(!r.status);
    BOOST_TEST(r.observations.empty());
    BOOST_TEST(!parsePingStatusDocument(text, kIcdArrival));
}

BOOST_AUTO_TEST_CASE(icd_status_example)
{
    const std::string text = fixture("icd-status.json");
    const TP arrival = utc(2020, 11, 23, 23, 2, 23);
    const auto r = parsePingDatagram(text, PingContext{arrival, TimeMode::utc});
    BOOST_TEST(r.rejected == 0u);
    BOOST_TEST(r.observations.empty());
    BOOST_REQUIRE(r.status);
    const auto check = [&](const DeviceStatus& s) {
        BOOST_TEST(s.guid == "754104714b10828b");
        BOOST_TEST(s.version == "1.0.0");
        BOOST_TEST(s.lat.value_or(0) == 48.088375);
        BOOST_TEST(s.lon.value_or(0) == -114.091881);
        BOOST_TEST(s.alt_m.value_or(0) == 946.7, boost::test_tools::tolerance(1e-9));
        BOOST_TEST(s.alt_type.value_or(-1) == 1);
        BOOST_TEST(s.gps_status.value_or(-1) == 4);
        BOOST_TEST(s.receiver_status.value_or(-1) == 0);
        BOOST_REQUIRE(s.device_time);
        BOOST_TEST(epochMs(*s.device_time) == epochMs(utc(2020, 11, 23, 23, 2, 22)) + 372);
        BOOST_TEST((s.received_at == arrival));
    };
    check(*r.status);
    auto doc = parsePingStatusDocument(text, arrival);
    BOOST_REQUIRE(doc);
    check(*doc);
    BOOST_TEST(!parsePingStatusDocument(R"({"aircraft":[]})", arrival));
    BOOST_TEST(!parsePingStatusDocument(R"({"status":{}})", arrival));
}

BOOST_AUTO_TEST_CASE(uat_detail_maps)
{
    const TP arrival = utc(2026, 11, 2, 12, 0, 0);
    const auto r = parsePingDatagram(fixture("uat-detail.json"), PingContext{arrival, TimeMode::utc});
    BOOST_TEST(r.rejected == 0u);
    BOOST_REQUIRE_EQUAL(r.observations.size(), 1u);
    const auto& o = r.observations[0];
    BOOST_TEST(o.id.address == 0xA1B2C3u);
    BOOST_TEST((o.id.type == AddressType::icao));
    BOOST_TEST((o.source == Source::uat_978));
    BOOST_TEST(o.alt_geom_ft.value_or(0) == 5000);
    BOOST_TEST(o.alt_baro_ft.value_or(0) == 5250);
    BOOST_TEST((o.on_ground == std::optional<bool>(true)));
    BOOST_TEST(o.emergency.value_or("") == "minfuel");
    BOOST_TEST(o.category.value_or("") == "B6");
    BOOST_TEST(o.squawk.value_or("") == "0007");
    BOOST_TEST(!o.lat);
    BOOST_TEST(!o.position_time);
    BOOST_REQUIRE_EQUAL(r.raw_frames.size(), 1u);
    BOOST_TEST((r.raw_frames[0].kind == RawFrame::Kind::uat_978));
    BOOST_TEST(r.raw_frames[0].hex == "00a1b2c31a2b3c4d5e6f708192a3b4c5d6e7");
    BOOST_TEST((r.raw_frames[0].time == arrival));  // no utcSync
}

BOOST_AUTO_TEST_CASE(secondary_altitude_never_overrides_primary)
{
    auto o = one(R"("altitudeMM":304800,"altitudeType":0,)"
                 R"("detail":{"secondaryAltType":0,"secondaryAltitudeMM":609600})");
    BOOST_TEST(o.alt_baro_ft.value_or(0) == 1000);
    BOOST_TEST(!o.alt_geom_ft);
    auto b = one(R"("altitudeMM":304800,"detail":{"secondaryAltitudeMM":609600})");  // types absent
    BOOST_TEST(b.alt_baro_ft.value_or(0) == 1000);
    BOOST_TEST(b.alt_geom_ft.value_or(0) == 2000);
}

BOOST_AUTO_TEST_CASE(bare_object_and_wrapped_array)
{
    BOOST_TEST(parse(R"({"icaoAddress":"ABCDEF"})").observations.size() == 1u);
    const auto r = parse(R"({"aircraft":[{"icaoAddress":"000001"},{"icaoAddress":"000002"},)"
                         R"({"icaoAddress":"000003"}]})");
    BOOST_REQUIRE_EQUAL(r.observations.size(), 3u);
    BOOST_TEST(r.observations[2].id.address == 3u);
    BOOST_TEST(r.rejected == 0u);
    BOOST_TEST(parse(R"({"aircraft":[]})").observations.empty());
    BOOST_TEST(parse(R"({"aircraft":[]})").rejected == 0u);
}

BOOST_AUTO_TEST_CASE(numbers_as_strings)
{
    auto o = one(R"("latDD":"42.1","lonDD":"-93.5","altitudeMM":"304800","altitudeType":"0",)"
                 R"("squawk":"1200","trafficSource":"1","horVelocityCMS":"1000","headingDE2":"9000",)"
                 R"("emitterType":"1","detail":{"airGroundState":"2","navIntegrity":"7"})");
    BOOST_TEST(o.lat.value_or(0) == 42.1);
    BOOST_TEST(o.lon.value_or(0) == -93.5);
    BOOST_TEST(o.alt_baro_ft.value_or(0) == 1000);
    BOOST_TEST(o.squawk.value_or("") == "1200");
    BOOST_TEST((o.source == Source::uat_978));
    BOOST_TEST(o.gs_kt.value_or(0) == 19.4384f, boost::test_tools::tolerance(1e-5f));
    BOOST_TEST(o.track_deg.value_or(0) == 90.0f);
    BOOST_TEST(o.category.value_or("") == "A1");
    BOOST_TEST((o.on_ground == std::optional<bool>(true)));
    BOOST_TEST(o.nic.value_or(0) == 7);
}

BOOST_AUTO_TEST_CASE(both_callsign_spellings)
{
    BOOST_TEST(one(R"("callsign":"CJT340 ")").callsign.value_or("") == "CJT340");
    BOOST_TEST(one(R"("Callsign":" UAL1   ")").callsign.value_or("") == "UAL1");
    BOOST_TEST(!one(R"("callsign":"        ")").callsign);
    BOOST_TEST(!one("").callsign);
}

BOOST_AUTO_TEST_CASE(vertical_rate_source)
{
    auto baro = one(R"("verVelocityCMS":-508)");  // verVelocitySrc absent -> baro
    BOOST_TEST(baro.baro_rate_fpm.value_or(0) == -1000.0f, boost::test_tools::tolerance(1e-5f));
    BOOST_TEST(!baro.geom_rate_fpm);
    auto geom = one(R"("verVelocityCMS":508,"detail":{"verVelocitySrc":1})");
    BOOST_TEST(geom.geom_rate_fpm.value_or(0) == 1000.0f, boost::test_tools::tolerance(1e-5f));
    BOOST_TEST(!geom.baro_rate_fpm);
}

BOOST_AUTO_TEST_CASE(missing_or_bad_icao_rejected)
{
    for (const char* bad : {R"({"latDD":1.0})", R"({"icaoAddress":"C0058"})", R"({"icaoAddress":"XYZ123"})",
                            R"({"icaoAddress":"C005861"})", R"({"icaoAddress":12})"}) {
        const auto r = parse(std::string(R"({"aircraft":[)") + bad + R"(,{"icaoAddress":"C00586"}]})");
        BOOST_TEST(r.rejected == 1u, bad);
        BOOST_REQUIRE_EQUAL(r.observations.size(), 1u);
        BOOST_TEST(r.observations[0].id.address == 0xC00586u);
        BOOST_TEST(parse(bad).rejected == 1u, bad);
    }
}

BOOST_AUTO_TEST_CASE(lat_without_lon_drops_both)
{
    auto o = one(R"("latDD":42.0,"timeStamp":"2026-11-02T12:00:00Z")");
    BOOST_TEST(!o.lat);
    BOOST_TEST(!o.lon);
    BOOST_TEST(!o.position_time);
    auto p = one(R"("lonDD":42.0)");
    BOOST_TEST(!p.lat);
    BOOST_TEST(!p.lon);
}

BOOST_AUTO_TEST_CASE(lat_out_of_range_drops_both)
{
    auto o = one(R"("latDD":91.0,"lonDD":10.0)");
    BOOST_TEST(!o.lat);
    BOOST_TEST(!o.lon);
    BOOST_TEST(!o.position_time);
    auto p = one(R"("latDD":10.0,"lonDD":-180.5)");
    BOOST_TEST(!p.lat);
    auto edge = one(R"("latDD":-90.0,"lonDD":180.0)");
    BOOST_TEST(edge.lat.value_or(0) == -90.0);
    BOOST_TEST(edge.lon.value_or(0) == 180.0);
    BOOST_TEST((edge.position_time.value() == edge.received_time));  // no timeStamp
}

BOOST_AUTO_TEST_CASE(emitter_types_all_24)
{
    const char* expected[24] = {"A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7", "B0", "B1", "B2", "B3",
                                "B4", "B5", "B6", "B7", "C0", "C1", "C2", "C3", "C4", "C5", "C6", "C7"};
    for (int v = 0; v < 24; ++v)
        BOOST_TEST(one(R"("emitterType":)" + std::to_string(v)).category.value_or("") == expected[v]);
    BOOST_TEST(!one(R"("emitterType":24)").category);
    BOOST_TEST(!one(R"("emitterType":39)").category);
    BOOST_TEST(!one(R"("emitterType":-1)").category);
}

BOOST_AUTO_TEST_CASE(address_qualifiers)
{
    struct Case {
        int q;
        AddressType type;
        Source source;
    };
    const Case cases[] = {
        {0, AddressType::icao, Source::adsb_1090},           {1, AddressType::non_icao, Source::adsb_1090},
        {2, AddressType::icao, Source::tisb},                {3, AddressType::tisb_trackfile, Source::tisb},
        {4, AddressType::non_icao, Source::adsb_1090},       {5, AddressType::non_icao, Source::adsb_1090},
        {6, AddressType::other, Source::adsb_1090},          {7, AddressType::other, Source::adsb_1090},
    };
    for (const auto& c : cases) {
        auto o = one(R"("detail":{"addressQualifier":)" + std::to_string(c.q) + "}");
        BOOST_TEST((o.id.type == c.type), "qualifier " << c.q);
        BOOST_TEST((o.source == c.source), "qualifier " << c.q);
    }
    auto absent = one("");
    BOOST_TEST((absent.id.type == AddressType::icao));
    BOOST_TEST((absent.source == Source::adsb_1090));
    auto uat = one(R"("trafficSource":1,"detail":{"addressQualifier":0})");
    BOOST_TEST((uat.source == Source::uat_978));
    auto uat_tisb = one(R"("trafficSource":1,"detail":{"addressQualifier":2})");
    BOOST_TEST((uat_tisb.source == Source::tisb));
    BOOST_TEST((one(R"("trafficSource":5)").source == Source::other));
}

BOOST_AUTO_TEST_CASE(squawk_raw_value_dropped)
{
    for (const char* raw : {"8000", "1288", "-1", "12345"}) {
        const auto r = parse(std::string(R"({"icaoAddress":"ABCDEF","squawk":)") + raw + "}");
        BOOST_REQUIRE_EQUAL(r.observations.size(), 1u);
        BOOST_TEST(!r.observations[0].squawk, raw);
        BOOST_TEST(r.bad_squawk, raw);
    }
    const auto ok = parse(R"({"icaoAddress":"ABCDEF","squawk":7700})");
    BOOST_TEST(ok.observations.at(0).squawk.value_or("") == "7700");
    BOOST_TEST(!ok.bad_squawk);
    BOOST_TEST(one(R"("squawk":0)").squawk.value_or("") == "0000");
}

BOOST_AUTO_TEST_CASE(emergency_states)
{
    const char* expected[7] = {"none", "general", "lifeguard", "minfuel", "nordo", "unlawful", "downed"};
    for (int v = 0; v < 7; ++v)
        BOOST_TEST(one(R"("detail":{"emergencyStatus":)" + std::to_string(v) + "}").emergency.value_or("") ==
                   expected[v]);
    BOOST_TEST(!one(R"("detail":{"emergencyStatus":7})").emergency);
}

BOOST_AUTO_TEST_CASE(no_detail_leaves_on_ground_unset)
{
    auto o = one(R"("latDD":1.0,"lonDD":2.0,"altitudeMM":1000)");
    BOOST_TEST(!o.on_ground);
    BOOST_TEST(!one(R"("detail":{"navIntegrity":8})").on_ground);
    BOOST_TEST((one(R"("detail":{"airGroundState":1})").on_ground == std::optional<bool>(false)));
}

BOOST_AUTO_TEST_CASE(oversize_and_not_json_rejected)
{
    const std::string head = R"({"icaoAddress":"ABCDEF","pad":")";
    const std::string tail = R"("})";
    std::string big = head + std::string(kMaxDatagramBytes + 1 - head.size() - tail.size(), 'x') + tail;
    BOOST_REQUIRE_EQUAL(big.size(), kMaxDatagramBytes + 1);
    auto r = parse(big);
    BOOST_TEST(r.rejected == 1u);
    BOOST_TEST(r.observations.empty());
    big.erase(head.size(), 1);  // exactly the limit: parsed
    BOOST_TEST(parse(big).observations.size() == 1u);

    for (const char* text : {"not json", "[]", R"({"other":1})", "", R"({"aircraft":5})",
                             R"({"aircraft":[1,2]})", R"({"status":5})"}) {
        PingResult bad;
        BOOST_CHECK_NO_THROW(bad = parse(text));
        BOOST_TEST(bad.rejected == 1u, text);
        BOOST_TEST(bad.observations.empty(), text);
        BOOST_TEST(!bad.status, text);
    }
}

BOOST_AUTO_TEST_CASE(raw_unknown_len_counted)
{
    const std::string hex31(62, 'a');
    auto r = parse(R"({"icaoAddress":"ABCDEF","detail":{"rawData":")" + hex31 + R"("}})");
    BOOST_TEST(r.observations.size() == 1u);
    BOOST_TEST(r.raw_unknown_len == 1u);
    BOOST_TEST(r.raw_frames.empty());

    auto s = parse(R"({"icaoAddress":"ABCDEF","detail":{"rawData":"5D4840D6202CC3"}})");
    BOOST_TEST(s.raw_unknown_len == 0u);
    BOOST_REQUIRE_EQUAL(s.raw_frames.size(), 1u);
    BOOST_TEST((s.raw_frames[0].kind == RawFrame::Kind::adsb_1090));
    BOOST_TEST(s.raw_frames[0].hex == "5d4840d6202cc3");

    auto l = parse(R"({"icaoAddress":"ABCDEF","detail":{"rawData":")" + std::string(68, '0') + R"("}})");
    BOOST_REQUIRE_EQUAL(l.raw_frames.size(), 1u);
    BOOST_TEST((l.raw_frames[0].kind == RawFrame::Kind::uat_978));

    for (const char* bad : {"5D4840D6202CC", "5D4840D6202CXX"}) {
        auto b = parse(std::string(R"({"icaoAddress":"ABCDEF","detail":{"rawData":")") + bad + R"("}})");
        BOOST_TEST(b.raw_unknown_len == 1u, bad);
        BOOST_TEST(b.raw_frames.empty(), bad);
    }
}

// ---- CAT021 ----

namespace {

using namespace skycued::asterix;

// A record with an address and a position, through the Task 1 encoder and
// decoder so `bytes` is what a device would send.
Cat021Record decoded(const Cat021Record& r)
{
    const auto blk = encodeBlock({r});
    const auto d = decodeBlock(blk.data(), blk.size(), Edition::e2_5);
    BOOST_REQUIRE(!d.error);
    BOOST_REQUIRE_EQUAL(d.records.size(), 1u);
    return d.records[0];
}

Cat021Record base()
{
    Cat021Record r;
    r.sac = 0;
    r.sic = 0;
    r.trd = Descriptor040{};
    r.address = 0xABCDEF;
    r.lat131 = static_cast<int32_t>(std::lround(42.5 / kLsb131Deg));
    r.lon131 = static_cast<int32_t>(std::lround(-93.25 / kLsb131Deg));
    return r;
}

const TP kCatArrival = utc(2026, 11, 2, 12, 0, 0);

std::optional<TrackObservation> map(const Cat021Record& r, Cat021Counts& counts,
                                    Cat021Context ctx = Cat021Context{kCatArrival, TimeMode::utc, {}, {}})
{
    return mapCat021Record(decoded(r), ctx, counts);
}

TrackObservation mapOk(const Cat021Record& r)
{
    Cat021Counts counts;
    auto o = map(r, counts);
    BOOST_REQUIRE(o);
    return *o;
}

uint32_t todRaw(double s) { return static_cast<uint32_t>(std::lround(s * 128)); }

std::string lowerHex(const std::vector<uint8_t>& bytes)
{
    static const char* d = "0123456789abcdef";
    std::string out;
    for (uint8_t b : bytes) {
        out += d[b >> 4];
        out += d[b & 15];
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(cat021_maps_reference_fields)
{
    Cat021Record r = base();
    r.lat130 = static_cast<int32_t>(std::lround(10.0 / kLsb130Deg));
    r.lon130 = static_cast<int32_t>(std::lround(20.0 / kLsb130Deg));
    r.fl145 = 1361;
    r.height140 = 5448;
    r.vector160 = Vector160{false, 1870, 47940};
    r.bvr155 = Rate{false, -10};
    r.gvr157 = Rate{false, 64};
    r.mode3a = 01305;
    r.ident = "CJT340  ";
    r.ecat = 5;
    r.status = Status200{};
    r.status->ps = 3;
    r.quality = Quality090{};
    r.quality->nucp_nic = 8;
    r.quality->has_ext1 = true;
    r.quality->nacp = 9;
    r.amplitude132 = -40;
    r.mops = Mops210{false, 2, 2};
    r.tod073 = todRaw(12 * 3600.0 + 1);

    const Cat021Record dec = decoded(r);
    Cat021Counts counts;
    auto o = mapCat021Record(dec, Cat021Context{kCatArrival, TimeMode::utc, {}, {}}, counts);
    BOOST_REQUIRE(o);
    BOOST_TEST(o->id.address == 0xABCDEFu);
    BOOST_TEST((o->id.type == AddressType::icao));
    BOOST_TEST((o->source == Source::adsb_1090));
    BOOST_TEST((o->received_time == kCatArrival));
    BOOST_TEST(o->lat.value_or(0) == 42.5, boost::test_tools::tolerance(1e-7));  // 131, not 130
    BOOST_TEST(o->lon.value_or(0) == -93.25, boost::test_tools::tolerance(1e-7));
    BOOST_TEST(o->alt_baro_ft.value_or(0) == 34025);
    BOOST_TEST(o->alt_geom_ft.value_or(0) == 34050);
    BOOST_TEST(o->gs_kt.value_or(0) == static_cast<float>(1870 * kKtPerGs160));
    BOOST_TEST(o->track_deg.value_or(0) == static_cast<float>(47940 * kDegPerTa160));
    BOOST_TEST(o->baro_rate_fpm.value_or(0) == -62.5f);
    BOOST_TEST(o->geom_rate_fpm.value_or(0) == 400.0f);
    BOOST_TEST(o->squawk.value_or("") == "1305");
    BOOST_TEST(o->callsign.value_or("") == "CJT340");
    BOOST_TEST(o->category.value_or("") == "A5");
    BOOST_TEST(o->emergency.value_or("") == "minfuel");
    BOOST_TEST(o->nic.value_or(0) == 8);
    BOOST_TEST(o->nac_p.value_or(0) == 9);
    BOOST_TEST(!o->rssi);  // 132 is dBm, never rssi
    BOOST_TEST(!o->on_ground);  // no 040 ext 1
    BOOST_TEST((o->position_time.value() == utc(2026, 11, 2, 12, 0, 1)));
    BOOST_TEST(o->raw_line == "cat021:" + lowerHex(dec.bytes));
    BOOST_TEST(counts.records_skipped == 0u);

    // 130 when 131 is absent.
    Cat021Record r130 = r;
    r130.lat131.reset();
    r130.lon131.reset();
    BOOST_TEST(mapOk(r130).lat.value_or(0) == 10.0, boost::test_tools::tolerance(1e-6));

    // 140 "greater than" and RE bits are ignored.
    Cat021Record re = r;
    re.height140 = 0x7FFF;
    re.bvr155 = Rate{true, 100};
    re.gvr157 = Rate{true, 100};
    re.vector160 = Vector160{true, 1870, 47940};
    auto ore = mapOk(re);
    BOOST_TEST(!ore.alt_geom_ft);
    BOOST_TEST(!ore.baro_rate_fpm);
    BOOST_TEST(!ore.geom_rate_fpm);
    BOOST_TEST(!ore.gs_kt);
    BOOST_TEST(!ore.track_deg);

    // Surface ground vector when 160 is absent.
    Cat021Record sgv = base();
    sgv.sgv = SurfaceVector{};
    sgv.sgv->gss_raw = 80;
    sgv.sgv->hgt_raw = 32;
    auto osgv = mapOk(sgv);
    BOOST_TEST(osgv.gs_kt.value_or(0) == 10.0f);
    BOOST_TEST(osgv.track_deg.value_or(0) == 90.0f);

    // Address types from ATP.
    for (auto [atp, type] : {std::pair{0, AddressType::icao}, std::pair{1, AddressType::icao},
                             std::pair{2, AddressType::non_icao}, std::pair{3, AddressType::non_icao},
                             std::pair{5, AddressType::other}}) {
        Cat021Record a = base();
        a.trd->atp = static_cast<uint8_t>(atp);
        BOOST_TEST((mapOk(a).id.type == type), "atp " << atp);
    }

    // Sources from LTT.
    for (auto [ltt, src] : {std::pair{2, Source::adsb_1090}, std::pair{1, Source::uat_978},
                            std::pair{3, Source::other}}) {
        Cat021Record m = base();
        m.mops = Mops210{false, 2, static_cast<uint8_t>(ltt)};
        BOOST_TEST((mapOk(m).source == src), "ltt " << ltt);
    }
    BOOST_TEST((mapOk(base()).source == Source::adsb_1090));  // no 210

    // Squawk with leading zeros, ident trimming.
    Cat021Record sq = base();
    sq.mode3a = 07;
    sq.ident = "        ";
    auto osq = mapOk(sq);
    BOOST_TEST(osq.squawk.value_or("") == "0007");
    BOOST_TEST(!osq.callsign);
    sq.ident = " N12  ";
    BOOST_TEST(mapOk(sq).callsign.value_or("") == "N12");

    // Emergencies from PS.
    const char* ps_names[7] = {"none", "general", "lifeguard", "minfuel", "nordo", "unlawful", "downed"};
    for (uint8_t ps = 0; ps < 7; ++ps) {
        Cat021Record e = base();
        e.status = Status200{};
        e.status->ps = ps;
        BOOST_TEST(mapOk(e).emergency.value_or("") == ps_names[ps]);
    }
    Cat021Record e7 = base();
    e7.status = Status200{};
    e7.status->ps = 7;
    BOOST_TEST(!mapOk(e7).emergency);

    // NACp needs 090 ext 1.
    Cat021Record q = base();
    q.quality = Quality090{};
    q.quality->nucp_nic = 6;
    auto oq = mapOk(q);
    BOOST_TEST(oq.nic.value_or(0) == 6);
    BOOST_TEST(!oq.nac_p);
}

BOOST_AUTO_TEST_CASE(cat021_category_table)
{
    const std::pair<int, const char*> table[] = {
        {1, "A1"},  {2, "A2"},  {3, "A3"},  {4, "A4"},  {5, "A5"},  {6, "A6"},  {10, "A7"},
        {11, "B1"}, {12, "B2"}, {16, "B3"}, {15, "B4"}, {13, "B6"}, {14, "B7"}, {20, "C1"},
        {21, "C2"}, {22, "C3"}, {23, "C4"}, {24, "C5"},
    };
    for (auto [v, s] : table)
        BOOST_TEST(category020(static_cast<uint8_t>(v)).value_or("") == s, "ecat " << v);
    for (int v : {0, 7, 8, 9, 17, 18, 19, 25, 255})
        BOOST_TEST(!category020(static_cast<uint8_t>(v)), "ecat " << v);
    Cat021Record r = base();
    r.ecat = 25;
    BOOST_TEST(!mapOk(r).category);
    r.ecat = 13;
    BOOST_TEST(mapOk(r).category.value_or("") == "B6");
}

BOOST_AUTO_TEST_CASE(cat021_flags)
{
    auto withExt1 = [](auto set) {
        Cat021Record r = base();
        r.trd->has_ext1 = true;
        set(*r.trd);
        return r;
    };
    auto withExt2 = [](auto set) {
        Cat021Record r = base();
        r.trd->has_ext1 = true;
        r.trd->has_ext2 = true;
        set(*r.trd);
        return r;
    };

    // Dropped and counted.
    {
        Cat021Counts c;
        BOOST_TEST(!map(withExt1([](Descriptor040& d) { d.sim = true; }), c));
        BOOST_TEST(!map(withExt1([](Descriptor040& d) { d.tst = true; }), c));
        Cat021Record rab = base();
        rab.trd->rab = true;
        BOOST_TEST(!map(rab, c));
        Cat021Record noaddr = base();
        noaddr.address.reset();
        BOOST_TEST(!map(noaddr, c));
        BOOST_TEST(c.records_skipped == 4u);
        BOOST_TEST(c.nogo_records == 0u);
    }
    // NOGO: mapped, counted.
    {
        Cat021Counts c;
        auto o = map(withExt2([](Descriptor040& d) { d.nogo = true; }), c);
        BOOST_TEST(o.has_value());
        BOOST_TEST(c.nogo_records == 1u);
        BOOST_TEST(c.records_skipped == 0u);
    }
    // Suspect positions: no position, other fields kept.
    {
        Cat021Record cl = withExt1([](Descriptor040& d) { d.cl = 1; });
        cl.fl145 = 100;
        auto o = mapOk(cl);
        BOOST_TEST(!o.lat);
        BOOST_TEST(!o.lon);
        BOOST_TEST(!o.position_time);
        BOOST_TEST(o.alt_baro_ft.value_or(0) == 2500);
        BOOST_TEST(mapOk(withExt1([](Descriptor040& d) { d.cl = 2; })).lat.has_value());
    }
    for (auto set : std::vector<void (*)(Descriptor040&)>{
             [](Descriptor040& d) { d.cpr = true; }, [](Descriptor040& d) { d.rcf = true; },
             [](Descriptor040& d) { d.ipc = true; }, [](Descriptor040& d) { d.llc = true; },
             [](Descriptor040& d) { d.ldpj = true; }}) {
        Cat021Record r = withExt2(set);
        r.mode3a = 01200;
        auto o = mapOk(r);
        BOOST_TEST(!o.lat);
        BOOST_TEST(!o.position_time);
        BOOST_TEST(o.squawk.value_or("") == "1200");
    }
    BOOST_TEST(mapOk(withExt2([](Descriptor040&) {})).lat.has_value());
    // ARC 3: 145 untrusted.
    {
        Cat021Record r = base();
        r.fl145 = 100;
        r.trd->arc = 3;
        BOOST_TEST(!mapOk(r).alt_baro_ft);
        r.trd->arc = 1;
        BOOST_TEST(mapOk(r).alt_baro_ft.value_or(0) == 2500);
    }
    // RC 1: position accepted.
    {
        Cat021Record r = base();
        r.trd->rc = true;
        BOOST_TEST(mapOk(r).lat.has_value());
    }
    // on_ground only from ext 1.
    BOOST_TEST(!mapOk(base()).on_ground);
    BOOST_TEST((mapOk(withExt1([](Descriptor040& d) { d.gbs = true; })).on_ground == std::optional<bool>(true)));
    BOOST_TEST((mapOk(withExt1([](Descriptor040&) {})).on_ground == std::optional<bool>(false)));
    // Out-of-range latitude: no position.
    {
        Cat021Record r = base();
        r.lat131 = static_cast<int32_t>(std::lround(-91.0 / kLsb131Deg));
        BOOST_TEST(!mapOk(r).lat);
    }
}

BOOST_AUTO_TEST_CASE(cat021_time_chain)
{
    const double noon = 12 * 3600.0;
    auto at = [](double s_after_noon) {
        return kCatArrival + std::chrono::duration_cast<system_clock::duration>(
                                 std::chrono::duration<double>(s_after_noon));
    };
    auto posTime = [](const Cat021Record& r) { return mapOk(r).position_time.value(); };

    Cat021Record r = base();
    r.tod071 = todRaw(noon + 1);
    r.tod073 = todRaw(noon + 2.25);
    r.tod077 = todRaw(noon + 3);
    r.tod075 = todRaw(noon + 4);
    BOOST_TEST((posTime(r) == at(1)));
    r.tod071.reset();
    BOOST_TEST((posTime(r) == at(2.25)));
    r.hp074 = Time074{0, 1u << 29};  // 0.5 s
    BOOST_TEST((posTime(r) == at(2.5)));
    r.hp074->fsi = 1;
    BOOST_TEST((posTime(r) == at(3.5)));
    r.hp074->fsi = 2;
    BOOST_TEST((posTime(r) == at(1.5)));
    r.hp074->fsi = 3;  // reserved: 073 alone
    BOOST_TEST((posTime(r) == at(2.25)));
    r.tod073.reset();  // 074 alone is meaningless
    BOOST_TEST((posTime(r) == at(3)));
    r.tod077.reset();
    BOOST_TEST((posTime(r) == at(4)));
    r.tod075.reset();
    r.hp074.reset();
    BOOST_TEST((posTime(r) == kCatArrival));

    // No time item: no offset.
    {
        Cat021Counts c;
        BOOST_REQUIRE(map(base(), c));
        BOOST_TEST(!c.clock_offset_s);
    }
    // Midnight: the date comes from arrival with the ±12 h rule.
    {
        Cat021Record m = base();
        m.tod073 = todRaw(0.5);
        Cat021Counts c;
        auto o = map(m, c, Cat021Context{utc(2026, 11, 2, 23, 59, 59), TimeMode::utc, {}, {}});
        BOOST_REQUIRE(o);
        BOOST_TEST((o->position_time.value() == utc(2026, 11, 3, 0, 0, 0, 500000000)));
        BOOST_TEST(c.clock_offset_s.value_or(0) == 1.5);
    }
    // Clamp.
    {
        Cat021Record far = base();
        far.tod073 = todRaw(noon + 30);
        Cat021Counts c;
        auto o = map(far, c);
        BOOST_REQUIRE(o);
        BOOST_TEST((o->position_time.value() == kCatArrival));
        BOOST_TEST(c.clock_rejects == 1u);
        BOOST_TEST(c.clock_offset_s.value_or(0) == 30.0);
    }
    // GPS time mode.
    {
        Cat021Record g = base();
        g.tod073 = todRaw(noon + 18);
        Cat021Counts c;
        auto o = map(g, c, Cat021Context{kCatArrival, TimeMode::gps, {}, {}});
        BOOST_REQUIRE(o);
        BOOST_TEST((o->position_time.value() == kCatArrival));
        BOOST_TEST(c.clock_offset_s.value_or(1) == 0.0);
        BOOST_TEST(c.clock_rejects == 0u);
    }
}

BOOST_AUTO_TEST_CASE(cat021_sac_sic_mismatch_counted_not_dropped)
{
    Cat021Counts c;
    BOOST_TEST(map(base(), c, Cat021Context{kCatArrival, TimeMode::utc, uint8_t{1}, uint8_t{2}}).has_value());
    BOOST_TEST(c.sac_sic_mismatch == 1u);
    Cat021Counts same;
    BOOST_TEST(map(base(), same, Cat021Context{kCatArrival, TimeMode::utc, uint8_t{0}, uint8_t{0}}).has_value());
    BOOST_TEST(same.sac_sic_mismatch == 0u);
    Cat021Counts only_sac;
    BOOST_TEST(map(base(), only_sac, Cat021Context{kCatArrival, TimeMode::utc, uint8_t{1}, {}}).has_value());
    BOOST_TEST(only_sac.sac_sic_mismatch == 0u);
}

BOOST_AUTO_TEST_CASE(input_counters_add)
{
    InputCounters a, b;
    a.messages = 2;
    a.clock_rejects = 1;
    b.messages = 3;
    b.status_fail = 4;
    b.raw_unknown_len = 5;
    a += b;
    BOOST_TEST(a.messages == 5u);
    BOOST_TEST(a.clock_rejects == 1u);
    BOOST_TEST(a.status_fail == 4u);
    BOOST_TEST(a.raw_unknown_len == 5u);
}
