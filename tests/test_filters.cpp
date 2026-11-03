#define BOOST_TEST_MODULE skycued_filters
#include <boost/test/unit_test.hpp>

#include "filters.hpp"
#include "geo.hpp"
#include "track_table.hpp"

#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>

using skycue::AddressType;
using skycue::TrackId;
using skycued::Decision;
using skycued::FilterConfig;
using skycued::FilterResult;
using skycued::FilterSet;
using skycued::IdList;
using skycued::ReceiverPosition;
using skycued::TrackState;

namespace {

constexpr double kPi = 3.14159265358979323846;
const ReceiverPosition kRx{40.0, -75.0, 100.0};

// Latitude `nm` nautical miles due north of the receiver (exact along a meridian).
double latNorthOf(double lat, double nm)
{
    return lat + (nm * skycued::geo::kMetresPerNm / skycued::geo::kEarthRadiusM) * 180.0 / kPi;
}

TrackState track(uint32_t address, double range_nm, std::optional<int32_t> baro_ft,
                 std::optional<int32_t> geom_ft = std::nullopt)
{
    TrackState t;
    t.id = TrackId{address, AddressType::icao};
    t.latest.id = t.id;
    t.latest.lat = latNorthOf(kRx.lat, range_nm);
    t.latest.lon = kRx.lon;
    t.latest.alt_baro_ft = baro_ft;
    t.latest.alt_geom_ft = geom_ft;
    return t;
}

FilterSet filters(double max_range_nm, double min_elevation_deg, IdList allow = IdList{}, IdList block = IdList{})
{
    return FilterSet(kRx, FilterConfig{max_range_nm, min_elevation_deg}, std::move(allow), std::move(block));
}

} // namespace

BOOST_AUTO_TEST_CASE(block_wins_over_allow)
{
    auto fs = filters(150.0, 0.0, IdList::parse("abc123\n"), IdList::parse("ABC123\n"));
    FilterResult r = fs.evaluate(track(0xabc123, 10.0, 30000));
    BOOST_TEST(static_cast<int>(r.decision) == static_cast<int>(Decision::blocked));
    BOOST_TEST(!skycued::isSend(r.decision));
    // Range/elevation are still computed for the UI.
    BOOST_TEST(r.range_nm == 10.0, boost::test_tools::tolerance(1e-6));
    BOOST_TEST(!std::isnan(r.elevation_deg));
}

BOOST_AUTO_TEST_CASE(allow_bypasses_range_and_elevation)
{
    auto fs = filters(150.0, 5.0, IdList::parse("abc123\n"));
    // 300 nm away at 1000 ft: far out of range and below the horizon.
    FilterResult r = fs.evaluate(track(0xabc123, 300.0, 1000));
    BOOST_TEST(static_cast<int>(r.decision) == static_cast<int>(Decision::allow_listed));
    BOOST_TEST(skycued::isSend(r.decision));
    BOOST_TEST(r.range_nm == 300.0, boost::test_tools::tolerance(1e-6));
    BOOST_TEST(r.elevation_deg < 0.0);

    // The same track without the list entry is out of range.
    auto plain = filters(150.0, 5.0);
    BOOST_TEST(static_cast<int>(plain.evaluate(track(0xabc123, 300.0, 1000)).decision) ==
               static_cast<int>(Decision::out_of_range));
}

BOOST_AUTO_TEST_CASE(range_filter_boundary)
{
    auto fs = filters(150.0, -90.0);
    FilterResult in = fs.evaluate(track(0x111111, 149.9, 40000));
    BOOST_TEST(static_cast<int>(in.decision) == static_cast<int>(Decision::send));
    BOOST_TEST(skycued::isSend(in.decision));
    BOOST_TEST(in.range_nm == 149.9, boost::test_tools::tolerance(1e-6));

    FilterResult out = fs.evaluate(track(0x111111, 150.1, 40000));
    BOOST_TEST(static_cast<int>(out.decision) == static_cast<int>(Decision::out_of_range));
    BOOST_TEST(!skycued::isSend(out.decision));
    BOOST_TEST(out.range_nm == 150.1, boost::test_tools::tolerance(1e-6));
}

BOOST_AUTO_TEST_CASE(elevation_filter)
{
    auto fs = filters(250.0, 2.0);

    // 20 nm at 40000 ft baro: roughly 18 degrees up.
    FilterResult high = fs.evaluate(track(0x222222, 20.0, 40000));
    BOOST_TEST(static_cast<int>(high.decision) == static_cast<int>(Decision::send));
    BOOST_TEST(!high.alt_is_geom);
    BOOST_TEST(high.elevation_deg > 15.0);

    // 20 nm at 1000 ft: below 2 degrees.
    FilterResult low = fs.evaluate(track(0x222222, 20.0, 1000));
    BOOST_TEST(static_cast<int>(low.decision) == static_cast<int>(Decision::below_elevation));
    BOOST_TEST(low.elevation_deg < 2.0);

    // Geometric altitude is preferred when present: low baro, high geom -> send.
    FilterResult geom = fs.evaluate(track(0x222222, 20.0, 1000, 40000));
    BOOST_TEST(static_cast<int>(geom.decision) == static_cast<int>(Decision::send));
    BOOST_TEST(geom.alt_is_geom);
    BOOST_TEST(geom.elevation_deg == high.elevation_deg, boost::test_tools::tolerance(0.5));

    // On the ground the receiver altitude is used, whatever the reported altitudes.
    TrackState ground = track(0x222222, 20.0, 40000, 40000);
    ground.latest.on_ground = true;
    FilterResult g = fs.evaluate(ground);
    BOOST_TEST(static_cast<int>(g.decision) == static_cast<int>(Decision::below_elevation));
    BOOST_TEST(!g.alt_is_geom);
    BOOST_TEST(g.elevation_deg < 0.0);
}

BOOST_AUTO_TEST_CASE(airborne_no_altitude_passes_elevation)
{
    // A high minimum elevation that any target at the receiver altitude fails.
    auto fs = filters(250.0, 10.0);
    FilterResult r = fs.evaluate(track(0x444444, 20.0, std::nullopt, std::nullopt));
    BOOST_TEST(static_cast<int>(r.decision) == static_cast<int>(Decision::send));
    BOOST_TEST(r.range_nm == 20.0, boost::test_tools::tolerance(1e-6));
    BOOST_TEST(std::isnan(r.elevation_deg));
    BOOST_TEST(!r.alt_is_geom);

    // Range still applies.
    FilterResult far = fs.evaluate(track(0x444444, 300.0, std::nullopt, std::nullopt));
    BOOST_TEST(static_cast<int>(far.decision) == static_cast<int>(Decision::out_of_range));

    // On the ground without altitude the receiver altitude is still used.
    TrackState ground = track(0x444444, 20.0, std::nullopt, std::nullopt);
    ground.latest.on_ground = true;
    BOOST_TEST(static_cast<int>(fs.evaluate(ground).decision) == static_cast<int>(Decision::below_elevation));
}

BOOST_AUTO_TEST_CASE(on_ground_unset_treated_as_airborne)
{
    auto fs = filters(250.0, 2.0);
    // Unset (the input did not say): the reported altitude is used, as for false.
    TrackState unset = track(0x555555, 20.0, 40000);
    BOOST_TEST(!unset.latest.on_ground.has_value());
    FilterResult u = fs.evaluate(unset);
    BOOST_TEST(static_cast<int>(u.decision) == static_cast<int>(Decision::send));
    BOOST_TEST(u.elevation_deg > 15.0);

    TrackState airborne = track(0x555555, 20.0, 40000);
    airborne.latest.on_ground = false;
    BOOST_TEST(fs.evaluate(airborne).elevation_deg == u.elevation_deg, boost::test_tools::tolerance(1e-9));

    // Unset without any altitude: no usable elevation, as for an airborne target.
    TrackState bare = track(0x555555, 20.0, std::nullopt, std::nullopt);
    BOOST_TEST(std::isnan(fs.evaluate(bare).elevation_deg));
}

BOOST_AUTO_TEST_CASE(no_position)
{
    auto fs = filters(150.0, 0.0);
    TrackState t = track(0x333333, 10.0, 30000);
    t.latest.lat.reset();
    t.latest.lon.reset();
    FilterResult r = fs.evaluate(t);
    BOOST_TEST(static_cast<int>(r.decision) == static_cast<int>(Decision::no_position));
    BOOST_TEST(!skycued::isSend(r.decision));
    BOOST_TEST(std::isnan(r.range_nm));
    BOOST_TEST(std::isnan(r.elevation_deg));

    // A track without a position is never forwarded: no_position is decided
    // before the curated lists, so an allow-listed track is not allow_listed.
    auto allowing = filters(150.0, 0.0, IdList::parse("333333"));
    FilterResult a = allowing.evaluate(t);
    BOOST_TEST(static_cast<int>(a.decision) == static_cast<int>(Decision::no_position));
    BOOST_TEST(!skycued::isSend(a.decision));
    BOOST_TEST(std::isnan(a.range_nm));
    BOOST_TEST(std::isnan(a.elevation_deg));

    // Likewise for a blocked track.
    auto blocking = filters(150.0, 0.0, IdList{}, IdList::parse("333333"));
    FilterResult b = blocking.evaluate(t);
    BOOST_TEST(static_cast<int>(b.decision) == static_cast<int>(Decision::no_position));
    BOOST_TEST(std::isnan(b.range_nm));
}

BOOST_AUTO_TEST_CASE(list_matches_hex_case_insensitive)
{
    IdList l = IdList::parse("  ABCDEF  \n# comment\n\n00a1b2 # trailing comment\n");
    BOOST_TEST(l.size() == 2u);
    BOOST_TEST(l.contains(TrackId{0xabcdef, AddressType::icao}, std::nullopt));
    BOOST_TEST(l.contains(TrackId{0x00a1b2, AddressType::icao}, std::nullopt));
    BOOST_TEST(!l.contains(TrackId{0xabcdee, AddressType::icao}, std::nullopt));
    // Hex matches for every address type (the operator listed what they saw).
    BOOST_TEST(l.contains(TrackId{0xabcdef, AddressType::tisb_trackfile}, std::nullopt));

    IdList lower = IdList::parse("abcdef\n");
    BOOST_TEST(lower.contains(TrackId{0xABCDEF, AddressType::icao}, std::nullopt));
}

BOOST_AUTO_TEST_CASE(list_matches_n_number_by_icao)
{
    IdList l = IdList::parse("N1\n");
    // No registration reported: matched through the derived ICAO address.
    BOOST_TEST(l.contains(TrackId{0xA00001, AddressType::icao}, std::nullopt));
    BOOST_TEST(!l.contains(TrackId{0xA00002, AddressType::icao}, std::nullopt));
    // The derivation applies to ICAO addresses only.
    BOOST_TEST(!l.contains(TrackId{0xA00001, AddressType::non_icao}, std::nullopt));
    // Lower case entries work too, and the registration string still matches.
    IdList lower = IdList::parse("n1\n");
    BOOST_TEST(lower.contains(TrackId{0xA00001, AddressType::icao}, std::nullopt));
    BOOST_TEST(lower.contains(TrackId{0x123456, AddressType::non_icao}, std::string("N1")));
}

BOOST_AUTO_TEST_CASE(list_matches_registration_string)
{
    IdList l = IdList::parse("G-ABCD\n");
    BOOST_TEST(l.size() == 1u);
    BOOST_TEST(l.contains(TrackId{0x400001, AddressType::icao}, std::string("G-ABCD")));
    BOOST_TEST(l.contains(TrackId{0x400001, AddressType::icao}, std::string(" g-abcd ")));
    BOOST_TEST(!l.contains(TrackId{0x400001, AddressType::icao}, std::string("G-ABCE")));
    BOOST_TEST(!l.contains(TrackId{0x400001, AddressType::icao}, std::nullopt));
}

BOOST_AUTO_TEST_CASE(list_parse_rejects_garbage_with_line)
{
    const std::string text = "abc123\n# fine\nnot valid!\n";
    try {
        IdList::parse(text);
        BOOST_FAIL("expected std::runtime_error");
    } catch (const std::runtime_error& e) {
        const std::string msg = e.what();
        BOOST_TEST(msg.find(":3:") != std::string::npos, msg);
        BOOST_TEST(msg.find("'not valid!'") != std::string::npos, msg);
    }
    // Too long, too short, bad characters, '#' glued to a token.
    BOOST_CHECK_THROW(IdList::parse("ABCDEFGHIJK\n"), std::runtime_error);
    BOOST_CHECK_THROW(IdList::parse("X\n"), std::runtime_error);
    BOOST_CHECK_THROW(IdList::parse("G_ABCD\n"), std::runtime_error);
    BOOST_CHECK_THROW(IdList::parse("abc123#x\n"), std::runtime_error);

    // loadFile names the path.
    try {
        IdList::loadFile("/nonexistent/skycue-list.txt");
        BOOST_FAIL("expected std::runtime_error");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()).find("/nonexistent/skycue-list.txt") != std::string::npos);
    }
}
