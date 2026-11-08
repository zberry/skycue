#define BOOST_TEST_MODULE decode_uat_frame
#include <boost/test/unit_test.hpp>

#include <cmath>
#include <string>

#include "decode/uat_frame.hpp"
#include "uat_test_support.hpp"

using namespace skycued;
using namespace skycued::decode::uat;
using skycued::testing::encodeUat;
using skycued::testing::toHex;
using skycued::testing::UatSpec;

namespace {
UatFrame frameOf(const UatSpec& s)
{
    UatFrame f;
    f.bytes = encodeUat(s);
    return f;
}
const double kLsb = 360.0 / 16777216.0;
}  // namespace

BOOST_AUTO_TEST_CASE(parse_line_plain_hex)
{
    UatSpec s;
    s.type = 0;
    const auto f = parseDownlinkLine(toHex(encodeUat(s)));
    BOOST_REQUIRE(f);
    BOOST_TEST(f->bytes.size() == 18u);
    BOOST_TEST(!f->rssi_dbfs);
    BOOST_TEST(!f->rs_errors);
    BOOST_TEST(parseDownlinkLine("-" + toHex(encodeUat(s)))->bytes == encodeUat(s));
}

BOOST_AUTO_TEST_CASE(parse_line_with_rssi_and_rs)
{
    const auto f = parseDownlinkLine("-" + toHex(encodeUat(UatSpec{})) + ";rssi=-12.0;rs=1;");
    BOOST_REQUIRE(f);
    BOOST_TEST(f->bytes.size() == 34u);
    BOOST_REQUIRE(f->rssi_dbfs);
    BOOST_TEST(*f->rssi_dbfs == -12.0);
    BOOST_REQUIRE(f->rs_errors);
    BOOST_TEST(*f->rs_errors == 1);
}

BOOST_AUTO_TEST_CASE(parse_line_rejects_odd_hex_and_bad_length)
{
    BOOST_TEST(!parseDownlinkLine(std::string(35, 'a')));
    BOOST_TEST(!parseDownlinkLine(std::string(36, 'g')));
    for (std::size_t n : {17u, 19u, 33u, 35u})
        BOOST_TEST(!parseDownlinkLine(std::string(n * 2, '0')));
    BOOST_TEST(!parseDownlinkLine("+" + std::string(36, '0')));
    BOOST_TEST(!parseDownlinkLine(""));
}

BOOST_AUTO_TEST_CASE(parse_line_unknown_keys_ignored)
{
    const auto f = parseDownlinkLine(toHex(encodeUat(UatSpec{})) + ";t=1700000000.5;foo=bar;rssi=-9.5;novalue;rs=x");
    BOOST_REQUIRE(f);
    BOOST_REQUIRE(f->rssi_dbfs);
    BOOST_TEST(*f->rssi_dbfs == -9.5);
    BOOST_TEST(!f->rs_errors);
}

BOOST_AUTO_TEST_CASE(header_fields_roundtrip)
{
    for (unsigned q = 0; q < 8; ++q) {
        UatSpec s;
        s.type = 2;
        s.qualifier = q;
        s.address = 0xA66EF1;
        const auto f = frameOf(s);
        BOOST_TEST(f.payload_type() == 2);
        BOOST_TEST(static_cast<unsigned>(f.qualifier()) == q);
        BOOST_TEST(f.address() == 0xA66EF1u);
    }
}

BOOST_AUTO_TEST_CASE(qualifier_table_rows)
{
    using skycue::AddressType;
    using skycue::Source;
    struct Row { AddressQualifier q; AddressType t; Source s; bool r; };
    const Row rows[] = {
        {AddressQualifier::adsb_icao, AddressType::icao, Source::uat_978, false},
        {AddressQualifier::adsb_selfassigned, AddressType::non_icao, Source::uat_978, false},
        {AddressQualifier::tisb_adsr_icao, AddressType::icao, Source::tisb, true},
        {AddressQualifier::tisb_trackfile, AddressType::tisb_trackfile, Source::tisb, true},
        {AddressQualifier::surface_vehicle, AddressType::non_icao, Source::uat_978, false},
        {AddressQualifier::fixed_beacon, AddressType::non_icao, Source::uat_978, false},
        {AddressQualifier::adsr_nonicao, AddressType::non_icao, Source::tisb, true},
    };
    for (const Row& r : rows) {
        const auto m = mapQualifier(r.q);
        BOOST_REQUIRE(m);
        BOOST_TEST((m->type == r.t));
        BOOST_TEST((m->source == r.s));
        BOOST_TEST(m->rebroadcast == r.r);
    }
    BOOST_TEST(!mapQualifier(AddressQualifier::reserved));
}

BOOST_AUTO_TEST_CASE(length_type_mismatch_unknown_type)
{
    UatSpec a;
    a.type = 0;
    BOOST_TEST(frameOf(a).lengthMatchesType());
    a.length = 34;
    BOOST_TEST(!frameOf(a).lengthMatchesType());
    UatSpec b;
    b.type = 3;
    BOOST_TEST(frameOf(b).lengthMatchesType());
    b.length = 18;
    const auto fb = frameOf(b);
    BOOST_TEST(!fb.lengthMatchesType());
    BOOST_TEST(!fb.hasMs());
    BOOST_TEST(!fb.hasAux());
    UatSpec c;
    c.type = 11;
    BOOST_TEST(!frameOf(c).lengthMatchesType());
    for (unsigned t = 1; t <= 10; ++t) {
        UatSpec d;
        d.type = t;
        BOOST_TEST(frameOf(d).lengthMatchesType());
    }
}

BOOST_AUTO_TEST_CASE(lat_lon_scaling)
{
    UatSpec s;
    s.lat = 37.45301;
    s.lon = -122.09613;
    s.nic = 9;
    const auto sv = decodeStateVector(frameOf(s));
    BOOST_REQUIRE(sv.lat);
    BOOST_REQUIRE(sv.lon);
    BOOST_TEST(std::fabs(*sv.lat - 37.45301) <= kLsb);
    BOOST_TEST(std::fabs(*sv.lon + 122.09613) <= kLsb);
    BOOST_TEST(sv.nic == 9);
}

BOOST_AUTO_TEST_CASE(southern_western_hemisphere_signs)
{
    UatSpec s;
    s.nic = 8;
    s.lat = -33.9461;
    s.lon = 151.1772;
    auto sv = decodeStateVector(frameOf(s));
    BOOST_TEST(std::fabs(*sv.lat + 33.9461) <= kLsb);
    BOOST_TEST(std::fabs(*sv.lon - 151.1772) <= kLsb);
    s.lat = 0.0001;
    s.lon = -0.0001;
    sv = decodeStateVector(frameOf(s));
    BOOST_TEST(*sv.lat > 0.0);
    BOOST_TEST(*sv.lon < 0.0);
    s.lat = -89.5;
    s.lon = -179.5;
    sv = decodeStateVector(frameOf(s));
    BOOST_TEST(std::fabs(*sv.lat + 89.5) <= kLsb);
    BOOST_TEST(std::fabs(*sv.lon + 179.5) <= kLsb);
}

BOOST_AUTO_TEST_CASE(latlon_zero_nic0_no_position)
{
    UatSpec s;
    s.zero_position = true;
    s.nic = 0;
    auto sv = decodeStateVector(frameOf(s));
    BOOST_TEST(!sv.lat);
    BOOST_TEST(!sv.lon);
    s.nic = 7;  // a genuine fix at 0,0 with a real NIC stays a position
    sv = decodeStateVector(frameOf(s));
    BOOST_REQUIRE(sv.lat);
    BOOST_TEST(*sv.lat == 0.0);
    BOOST_TEST(*sv.lon == 0.0);
}

BOOST_AUTO_TEST_CASE(altitude_formula)
{
    UatSpec s;
    s.alt_ft = -1000;  // raw 1
    BOOST_TEST(*decodeStateVector(frameOf(s)).altitude_ft == -1000);
    s.alt_ft = 0;  // raw 41
    BOOST_TEST(*decodeStateVector(frameOf(s)).altitude_ft == 0);
    s.alt_ft = 101350;  // raw 4095
    BOOST_TEST(*decodeStateVector(frameOf(s)).altitude_ft == 101350);
    s.alt_ft = 35000;
    BOOST_TEST(*decodeStateVector(frameOf(s)).altitude_ft == 35000);
}

BOOST_AUTO_TEST_CASE(altitude_raw0_unavailable)
{
    UatSpec s;
    BOOST_TEST(!decodeStateVector(frameOf(s)).altitude_ft);
}

BOOST_AUTO_TEST_CASE(alt_type_bit_selects)
{
    UatSpec s;
    s.alt_ft = 5000;
    BOOST_TEST((decodeStateVector(frameOf(s)).alt_type == AltType::baro));
    s.alt_geometric = true;
    const auto sv = decodeStateVector(frameOf(s));
    BOOST_TEST((sv.alt_type == AltType::geometric));
    BOOST_TEST(*sv.altitude_ft == 5000);
}

BOOST_AUTO_TEST_CASE(ag0_airborne_velocity)
{
    UatSpec s;
    s.ag = 0;
    s.ns_kt = -100;
    s.ew_kt = 65;
    const auto sv = decodeStateVector(frameOf(s));
    BOOST_TEST((sv.ag == StateVector::Ag::airborne_subsonic));
    BOOST_REQUIRE(sv.gs_kt);
    BOOST_REQUIRE(sv.track_deg);
    BOOST_TEST(std::fabs(*sv.gs_kt - 119.3) < 0.5);
    BOOST_TEST(std::fabs(*sv.track_deg - 147.0) < 0.5);
    UatSpec n;  // none given: raw 0, unavailable
    const auto nv = decodeStateVector(frameOf(n));
    BOOST_TEST(!nv.gs_kt);
    BOOST_TEST(!nv.track_deg);
}

BOOST_AUTO_TEST_CASE(ag1_supersonic_x4)
{
    UatSpec s;
    s.ag = 1;
    s.ns_kt = -400;
    s.ew_kt = 260;
    const auto sv = decodeStateVector(frameOf(s));
    BOOST_TEST((sv.ag == StateVector::Ag::airborne_supersonic));
    BOOST_TEST(std::fabs(*sv.gs_kt - 477.0) < 2.0);
    BOOST_TEST(std::fabs(*sv.track_deg - 147.0) < 0.5);
}

BOOST_AUTO_TEST_CASE(ag2_uses_ground_layout)
{
    UatSpec s;
    s.ag = 2;
    s.ground_speed_kt = 25;
    s.track_type = 1;
    s.track_deg = 90.0;
    // bytes that would be airborne velocity must not leak into the ground reading
    const auto sv = decodeStateVector(frameOf(s));
    BOOST_TEST((sv.ag == StateVector::Ag::ground));
    BOOST_REQUIRE(sv.gs_kt);
    BOOST_TEST(*sv.gs_kt == 25.0f);
    BOOST_REQUIRE(sv.track_deg);
    BOOST_TEST(std::fabs(*sv.track_deg - 90.0) < 0.5);
    BOOST_TEST(!sv.vrate_fpm);
}

BOOST_AUTO_TEST_CASE(ag3_reserved_no_velocity)
{
    UatSpec s;
    s.ag = 3;
    s.lat = 10.0;
    s.lon = 10.0;
    s.nic = 5;
    const auto sv = decodeStateVector(frameOf(s));
    BOOST_TEST((sv.ag == StateVector::Ag::reserved));
    BOOST_TEST(!sv.gs_kt);
    BOOST_TEST(!sv.track_deg);
    BOOST_TEST(!sv.vrate_fpm);
}

BOOST_AUTO_TEST_CASE(ground_track_type_not_track_gives_no_track)
{
    UatSpec s;
    s.ag = 2;
    s.ground_speed_kt = 12;
    s.track_deg = 200.0;
    for (unsigned t : {0u, 2u, 3u}) {
        s.track_type = t;
        const auto sv = decodeStateVector(frameOf(s));
        BOOST_TEST(*sv.gs_kt == 12.0f);
        BOOST_TEST(!sv.track_deg);
    }
}

BOOST_AUTO_TEST_CASE(vrate_sign_source_and_64fpm)
{
    UatSpec s;
    s.ns_kt = 100;
    s.ew_kt = 0;
    s.vrate_fpm = 1280;
    auto sv = decodeStateVector(frameOf(s));
    BOOST_REQUIRE(sv.vrate_fpm);
    BOOST_TEST(*sv.vrate_fpm == 1280);
    BOOST_TEST(*sv.vrate_geometric == false);
    s.vrate_fpm = -640;
    s.vrate_geometric = true;
    sv = decodeStateVector(frameOf(s));
    BOOST_TEST(*sv.vrate_fpm == -640);
    BOOST_TEST(*sv.vrate_geometric == true);
    s.vrate_fpm = 0;  // raw 1: level flight, not unavailable
    BOOST_TEST(*decodeStateVector(frameOf(s)).vrate_fpm == 0);
    s.vrate_fpm.reset();
    BOOST_TEST(!decodeStateVector(frameOf(s)).vrate_fpm);
}

BOOST_AUTO_TEST_CASE(emitter_category_letters)
{
    const struct { unsigned v; const char* s; } cases[] = {{0, "A0"}, {1, "A1"}, {9, "B1"}, {17, "C1"}, {25, "D1"}, {31, "D7"}, {39, ""}};
    for (const auto& c : cases) {
        UatSpec s;
        s.type = 1;
        s.emitter = c.v;
        const auto ms = decodeModeStatus(frameOf(s));
        BOOST_REQUIRE(ms);
        BOOST_TEST(ms->emitter_category == c.s);
    }
}

BOOST_AUTO_TEST_CASE(callsign_padding_trimmed)
{
    UatSpec s;
    s.type = 1;
    s.call = "N123AB";
    const auto ms = decodeModeStatus(frameOf(s));
    BOOST_REQUIRE(ms->callsign);
    BOOST_TEST(*ms->callsign == "N123AB");
    BOOST_TEST(!ms->squawk);
    s.call = "UAL1234";
    BOOST_TEST(*decodeModeStatus(frameOf(s))->callsign == "UAL1234");
    s.call = "        ";
    BOOST_TEST(!decodeModeStatus(frameOf(s))->callsign);
}

BOOST_AUTO_TEST_CASE(callsign_reserved_symbol_dropped)
{
    UatSpec s;
    s.type = 1;
    s.call = "AB?D";
    const auto ms = decodeModeStatus(frameOf(s));
    BOOST_REQUIRE(ms);
    BOOST_TEST(!ms->callsign);
    BOOST_TEST(!ms->squawk);
}

BOOST_AUTO_TEST_CASE(csid0_octal_squawk_else_none)
{
    UatSpec s;
    s.type = 3;
    s.csid = false;
    s.call = "7700";
    auto ms = decodeModeStatus(frameOf(s));
    BOOST_REQUIRE(ms->squawk);
    BOOST_TEST(*ms->squawk == "7700");
    BOOST_TEST(!ms->callsign);
    s.call = "1289";  // 8 and 9 are not octal
    ms = decodeModeStatus(frameOf(s));
    BOOST_TEST(!ms->squawk);
    s.call = "N123AB";
    BOOST_TEST(!decodeModeStatus(frameOf(s))->squawk);
}

BOOST_AUTO_TEST_CASE(emergency_0_to_6_names)
{
    const char* const names[] = {"none", "general", "medical", "minimum_fuel", "no_comms",
                                 "unlawful_interference", "downed_aircraft"};
    for (unsigned e = 0; e <= 6; ++e) {
        UatSpec s;
        s.type = 1;
        s.emergency = e;
        const auto ms = decodeModeStatus(frameOf(s));
        BOOST_REQUIRE(ms->emergency);
        BOOST_TEST(*ms->emergency == names[e]);
    }
    UatSpec s;
    s.type = 1;
    s.emergency = 7;
    BOOST_TEST(!decodeModeStatus(frameOf(s))->emergency);
}

BOOST_AUTO_TEST_CASE(nacp_value)
{
    UatSpec s;
    s.type = 1;
    s.nacp = 9;
    BOOST_TEST(*decodeModeStatus(frameOf(s))->nac_p == 9);
}

BOOST_AUTO_TEST_CASE(aux_sv_absent_for_types_3_4_7_to_10)
{
    for (unsigned t : {0u, 3u, 4u, 7u, 8u, 9u, 10u}) {
        UatSpec s;
        s.type = t;
        s.aux_alt_ft = 4000;
        const auto f = frameOf(s);
        BOOST_TEST(!f.hasAux());
        BOOST_TEST(!decodeAuxSv(f));
    }
}

BOOST_AUTO_TEST_CASE(aux_secondary_altitude_formula)
{
    for (unsigned t : {1u, 2u, 5u, 6u}) {
        UatSpec s;
        s.type = t;
        s.aux_alt_ft = 12350;
        const auto f = frameOf(s);
        BOOST_TEST(f.hasAux());
        BOOST_REQUIRE(decodeAuxSv(f));
        BOOST_TEST(*decodeAuxSv(f) == 12350);
        s.aux_alt_ft = -1000;
        BOOST_TEST(*decodeAuxSv(frameOf(s)) == -1000);
        s.aux_alt_ft.reset();  // raw 0
        BOOST_TEST(!decodeAuxSv(frameOf(s)));
    }
}

BOOST_AUTO_TEST_CASE(ms_absent_for_types_0_2_4_5_6)
{
    for (unsigned t : {0u, 2u, 4u, 5u, 6u}) {
        UatSpec s;
        s.type = t;
        BOOST_TEST(!frameOf(s).hasMs());
        BOOST_TEST(!decodeModeStatus(frameOf(s)));
    }
    UatSpec s;
    s.type = 3;
    BOOST_TEST(frameOf(s).hasMs());
}

BOOST_AUTO_TEST_CASE(short_byte_vector_does_not_read_out_of_bounds)
{
    for (std::size_t n : {0u, 1u, 3u, 4u, 17u}) {
        UatFrame f;
        f.bytes.assign(n, 0xFF);
        (void)f.payload_type();
        (void)f.qualifier();
        BOOST_TEST(f.address() == (n >= 4 ? 0xFFFFFFu : 0u));
        BOOST_TEST(!f.lengthMatchesType());
        const auto sv = decodeStateVector(f);
        BOOST_TEST(!sv.lat);
        BOOST_TEST(!sv.altitude_ft);
        BOOST_TEST(!decodeModeStatus(f));
        BOOST_TEST(!decodeAuxSv(f));
    }
}

BOOST_AUTO_TEST_CASE(parse_line_rs_not_integer_ignored)
{
    const auto f = parseDownlinkLine(toHex(encodeUat(UatSpec{})) + ";rs=1e30");
    BOOST_REQUIRE(f);
    BOOST_TEST(!f->rs_errors);
}

BOOST_AUTO_TEST_CASE(csid0_squawk_with_filler_symbols)
{
    // A frame as dump978-fa demodulated it from the sample: flight plan id "0322", the
    // other four symbols 37 (the golden prints flightplan_id 0322 for it).
    const auto f = parseDownlinkLine("08a66ef1353ae55263ac04f9117c2ba03f0c830cf5ed2d0bbaa4c0a0000590000000;");
    BOOST_REQUIRE(f);
    const auto ms = decodeModeStatus(*f);
    BOOST_REQUIRE(ms);
    BOOST_REQUIRE(ms->squawk);
    BOOST_TEST(*ms->squawk == "0322");
    BOOST_TEST(!ms->callsign);
}
