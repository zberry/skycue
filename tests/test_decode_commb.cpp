#define BOOST_TEST_MODULE decode_commb
#include <boost/test/unit_test.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "decode/acas.hpp"
#include "decode/commb.hpp"
#include "decode/frame.hpp"
#include "decode_test_support.hpp"

using namespace skycued;
using namespace skycued::decode;
using testing::Mb;

namespace {

// Hand-built registers. Mb bit numbers are the amendment's (1-based).

Mb bds40Mb()
{
    Mb m;  // MCP 36000 ft, FMS 35008 ft (16 ft LSB), QNH 1013.2, VNAV + ALTHOLD
    m.at(1, 1, 1).at(2, 12, 36000 / 16);
    m.at(14, 1, 1).at(15, 12, 35008 / 16);
    m.at(27, 1, 1).at(28, 12, 2132);
    m.at(48, 1, 1).at(49, 1, 1).at(50, 1, 1);
    return m;
}

Mb bds50Mb()
{
    Mb m;  // roll -12.3, track 271.4, GS 452, rate -0.5 deg/s, TAS 470
    m.at(1, 1, 1).sat(2, 10, -70);
    m.at(12, 1, 1).at(13, 11, 1544);
    m.at(24, 1, 1).at(25, 10, 226);
    m.at(35, 1, 1).sat(36, 10, -16);
    m.at(46, 1, 1).at(47, 10, 235);
    return m;
}

Mb bds60Mb()
{
    Mb m;  // heading 95.6, IAS 280, Mach 0.78, baro -1216, inertial -1184
    m.at(1, 1, 1).at(2, 11, 544);
    m.at(13, 1, 1).at(14, 10, 280);
    m.at(24, 1, 1).at(25, 10, 195);
    m.at(35, 1, 1).sat(36, 10, -38);
    m.at(46, 1, 1).sat(47, 10, -37);
    return m;
}

// Passes both the 5,0 and the 6,0 checks: track 225 deg / GS 200 / TAS 100 as
// 5,0, heading ~0.2 / IAS 256 / Mach 0.4 / rates 0 and 1600 as 6,0; fails 4,0.
Mb ambiguousMb()
{
    Mb m;
    m.at(1, 1, 1).at(12, 1, 1).at(13, 1, 1).at(14, 10, 256);
    m.at(24, 1, 1).at(25, 10, 100);
    m.at(35, 1, 1);
    m.at(46, 1, 1).at(47, 10, 50);
    return m;
}

Frame asFrame(const testing::Bytes& b)
{
    auto f = classify(b);
    BOOST_REQUIRE(f.has_value());
    return *f;
}

}  // namespace

BOOST_AUTO_TEST_CASE(bds40_all_fields)
{
    const auto b = decodeBds40(bds40Mb().v);
    BOOST_REQUIRE(b.has_value());
    BOOST_REQUIRE(b->sel_alt_mcp_ft && b->sel_alt_fms_ft && b->qnh_mb);
    BOOST_CHECK_EQUAL(*b->sel_alt_mcp_ft, 36000);
    BOOST_CHECK_EQUAL(*b->sel_alt_fms_ft, 35008);
    BOOST_CHECK_SMALL(*b->qnh_mb - 1013.2f, 0.01f);
    BOOST_CHECK(b->modes_valid);
    BOOST_CHECK(b->vnav);
    BOOST_CHECK(b->althold);
    BOOST_CHECK(!b->approach);
}

BOOST_AUTO_TEST_CASE(bds40_partial_and_baro_limits)
{
    Mb m;
    m.at(27, 1, 1).at(28, 12, 0);  // 800.0 mb, the lowest
    auto b = decodeBds40(m.v);
    BOOST_REQUIRE(b.has_value());
    BOOST_CHECK_SMALL(*b->qnh_mb - 800.0f, 0.001f);
    BOOST_CHECK(!b->sel_alt_mcp_ft.has_value());
    BOOST_CHECK(!b->modes_valid);
    m.at(28, 12, 4095);  // 1209.5 mb, the highest a 12-bit field holds
    b = decodeBds40(m.v);
    BOOST_REQUIRE(b.has_value());
    BOOST_CHECK_SMALL(*b->qnh_mb - 1209.5f, 0.001f);
}

BOOST_AUTO_TEST_CASE(bds40_status_zero_requires_zero_value)
{
    Mb m = bds40Mb();
    m.at(1, 1, 0);  // MCP status off, value still set
    BOOST_CHECK(!decodeBds40(m.v).has_value());
    Mb n = bds40Mb();
    n.at(48, 1, 0);  // mode status off with VNAV/ALTHOLD set
    BOOST_CHECK(!decodeBds40(n.v).has_value());
    Mb none;  // all zero: every field's status is 0 and so is its value, but at least one status must be set
    BOOST_CHECK(!decodeBds40(none.v).has_value());
}

BOOST_AUTO_TEST_CASE(bds40_reserved_bits_reject)
{
    for (unsigned bit : {40u, 47u, 52u, 53u}) {
        Mb m = bds40Mb();
        m.at(bit, 1, 1);
        BOOST_CHECK_MESSAGE(!decodeBds40(m.v).has_value(), "reserved bit " << bit);
    }
}

BOOST_AUTO_TEST_CASE(bds50_all_fields)
{
    const auto b = decodeBds50(bds50Mb().v);
    BOOST_REQUIRE(b.has_value());
    BOOST_REQUIRE(b->roll_deg && b->track_deg && b->gs_kt && b->track_rate_dps && b->tas_kt);
    BOOST_CHECK_SMALL(*b->roll_deg + 12.3f, 45.0f / 256.0f);
    BOOST_CHECK_SMALL(*b->track_deg - 271.4f, 90.0f / 512.0f);
    BOOST_CHECK_SMALL(*b->gs_kt - 452.0f, 2.0f);
    BOOST_CHECK_SMALL(*b->track_rate_dps + 0.5f, 8.0f / 256.0f);
    BOOST_CHECK_SMALL(*b->tas_kt - 470.0f, 2.0f);
}

BOOST_AUTO_TEST_CASE(bds50_gs_tas_inconsistent_rejects)
{
    Mb m;
    m.at(24, 1, 1).at(25, 10, 50);   // GS 100
    m.at(46, 1, 1).at(47, 10, 225);  // TAS 450
    BOOST_CHECK(!decodeBds50(m.v).has_value());
    m.at(47, 10, 100);  // TAS 200: within 200 kt of GS
    BOOST_CHECK(decodeBds50(m.v).has_value());
}

BOOST_AUTO_TEST_CASE(bds50_roll_over_50_rejects)
{
    Mb m;
    m.at(1, 1, 1).sat(2, 10, 300);  // 52.7 deg
    BOOST_CHECK(!decodeBds50(m.v).has_value());
    m.sat(2, 10, -300);
    BOOST_CHECK(!decodeBds50(m.v).has_value());
    m.sat(2, 10, 280);  // 49.2 deg
    BOOST_CHECK(decodeBds50(m.v).has_value());
}

BOOST_AUTO_TEST_CASE(bds50_range_and_status_rules)
{
    Mb gs;
    gs.at(24, 1, 1).at(25, 10, 301);  // 602 kt
    BOOST_CHECK(!decodeBds50(gs.v).has_value());
    Mb tas;
    tas.at(46, 1, 1).at(47, 10, 251);  // 502 kt
    BOOST_CHECK(!decodeBds50(tas.v).has_value());
    Mb rate;
    rate.at(35, 1, 1).sat(36, 10, 330);  // 10.3 deg/s
    BOOST_CHECK(!decodeBds50(rate.v).has_value());
    Mb off = bds50Mb();
    off.at(12, 1, 0);  // track status off, value set
    BOOST_CHECK(!decodeBds50(off.v).has_value());
}

BOOST_AUTO_TEST_CASE(bds60_all_fields)
{
    const auto b = decodeBds60(bds60Mb().v, std::nullopt);
    BOOST_REQUIRE(b.has_value());
    BOOST_REQUIRE(b->heading_mag_deg && b->ias_kt && b->mach && b->baro_rate_fpm && b->inertial_rate_fpm);
    BOOST_CHECK_SMALL(*b->heading_mag_deg - 95.6f, 90.0f / 512.0f);
    BOOST_CHECK_EQUAL(*b->ias_kt, 280);
    BOOST_CHECK_SMALL(*b->mach - 0.78f, 0.004f);
    BOOST_CHECK_SMALL(*b->baro_rate_fpm + 1216, 33);
    BOOST_CHECK_SMALL(*b->inertial_rate_fpm + 1184, 33);
}

BOOST_AUTO_TEST_CASE(bds60_rates_disagree_rejects)
{
    Mb m = bds60Mb();
    m.sat(36, 10, -94);  // baro -3008
    m.sat(47, 10, 0);    // inertial 0
    BOOST_CHECK(!decodeBds60(m.v, std::nullopt).has_value());
}

BOOST_AUTO_TEST_CASE(bds60_ias_mach_vs_altitude_rejects)
{
    Mb m = bds60Mb();
    BOOST_CHECK(decodeBds60(m.v, 35000).has_value());  // 280 kt IAS is ~Mach 0.87 there: 0.78 is within 20 %
    m.at(25, 10, 75);                                  // Mach 0.30
    BOOST_CHECK(decodeBds60(m.v, std::nullopt).has_value());
    BOOST_CHECK(!decodeBds60(m.v, 35000).has_value());
}

BOOST_AUTO_TEST_CASE(bds60_range_rules)
{
    Mb ias;
    ias.at(13, 1, 1).at(14, 10, 501);
    BOOST_CHECK(!decodeBds60(ias.v, std::nullopt).has_value());
    Mb mach;
    mach.at(24, 1, 1).at(25, 10, 251);  // 1.004
    BOOST_CHECK(!decodeBds60(mach.v, std::nullopt).has_value());
    Mb rate;
    rate.at(35, 1, 1).sat(36, 10, 190);  // 6080 ft/min
    BOOST_CHECK(!decodeBds60(rate.v, std::nullopt).has_value());
    Mb off = bds60Mb();
    off.at(13, 1, 0);  // IAS status off, value set
    BOOST_CHECK(!decodeBds60(off.v, std::nullopt).has_value());
}

BOOST_AUTO_TEST_CASE(infer_unique_register)
{
    CommbContext ctx;
    const auto r6 = inferCommb(bds60Mb().v, ctx);
    BOOST_CHECK(r6.kind == CommbKind::bds60);
    BOOST_CHECK(r6.b60.has_value());
    BOOST_CHECK(!r6.b50.has_value() && !r6.b40.has_value());
    const auto r4 = inferCommb(bds40Mb().v, ctx);
    BOOST_CHECK(r4.kind == CommbKind::bds40);
    BOOST_CHECK(r4.b40.has_value());
    const auto r5 = inferCommb(bds50Mb().v, ctx);
    BOOST_CHECK(r5.kind == CommbKind::bds50);
    BOOST_CHECK(r5.b50.has_value());
}

BOOST_AUTO_TEST_CASE(ambiguous_without_context_sets_nothing)
{
    const Mb m = ambiguousMb();
    BOOST_REQUIRE(decodeBds50(m.v).has_value());
    BOOST_REQUIRE(decodeBds60(m.v, std::nullopt).has_value());
    BOOST_REQUIRE(!decodeBds40(m.v).has_value());
    const auto r = inferCommb(m.v, CommbContext{});
    BOOST_CHECK(r.kind == CommbKind::ambiguous);
    BOOST_CHECK(!r.b40 && !r.b50 && !r.b60);
}

BOOST_AUTO_TEST_CASE(infer_tie_break_by_track)
{
    const Mb m = ambiguousMb();
    CommbContext ctx;
    ctx.track_deg = 230.0f;  // the 5,0 track is 225
    auto r = inferCommb(m.v, ctx);
    BOOST_CHECK(r.kind == CommbKind::bds50);
    BOOST_CHECK(r.b50.has_value() && !r.b60.has_value());
    ctx.track_deg = 100.0f;  // contradicts 5,0; nothing supports 6,0
    r = inferCommb(m.v, ctx);
    BOOST_CHECK(r.kind == CommbKind::bds60);
    BOOST_CHECK(r.b60.has_value());
}

BOOST_AUTO_TEST_CASE(infer_tie_break_by_ground_speed_and_baro_rate)
{
    const Mb m = ambiguousMb();
    CommbContext gs;
    gs.gs_kt = 185.0f;  // the 5,0 ground speed is 200
    BOOST_CHECK(inferCommb(m.v, gs).kind == CommbKind::bds50);
    gs.gs_kt = 400.0f;
    BOOST_CHECK(inferCommb(m.v, gs).kind == CommbKind::bds60);
    CommbContext rate;
    rate.baro_rate_fpm = 300;  // the 6,0 baro rate is 0
    BOOST_CHECK(inferCommb(m.v, rate).kind == CommbKind::bds60);
    rate.baro_rate_fpm = 2000;  // contradicts 6,0, and 5,0 has no evidence against it
    BOOST_CHECK(inferCommb(m.v, rate).kind == CommbKind::bds50);
    CommbContext both;  // 5,0 and 6,0 each contradicted: nothing set
    both.track_deg = 100.0f;
    both.baro_rate_fpm = 2000;
    BOOST_CHECK(inferCommb(m.v, both).kind == CommbKind::ambiguous);
}

BOOST_AUTO_TEST_CASE(bds40_mode_target_source_status)
{
    Mb m = bds40Mb();
    m.at(55, 2, 1);  // target altitude source set while its status (bit 54) is 0
    BOOST_CHECK(!decodeBds40(m.v).has_value());
    m.at(54, 1, 1);
    BOOST_CHECK(decodeBds40(m.v).has_value());
    Mb n = bds40Mb();
    n.at(56, 1, 1);
    BOOST_CHECK(!decodeBds40(n.v).has_value());
}

BOOST_AUTO_TEST_CASE(bds50_each_status_zero_requires_zero_value)
{
    // status bit, first value bit, width
    const unsigned f[][3] = {{1, 2, 10}, {24, 25, 10}, {35, 36, 10}, {46, 47, 10}};
    for (const auto& x : f) {
        Mb m = bds50Mb();
        BOOST_REQUIRE(decodeBds50(m.v).has_value());
        m.at(x[0], 1, 0);
        BOOST_CHECK_MESSAGE(!decodeBds50(m.v).has_value(), "5,0 status bit " << x[0]);
        m.at(x[1], x[2], 0);  // value cleared too: accepted
        BOOST_CHECK_MESSAGE(decodeBds50(m.v).has_value(), "5,0 cleared field at " << x[0]);
    }
}

BOOST_AUTO_TEST_CASE(bds60_each_status_zero_requires_zero_value)
{
    const unsigned f[][3] = {{1, 2, 11}, {24, 25, 10}, {35, 36, 10}, {46, 47, 10}};
    for (const auto& x : f) {
        Mb m = bds60Mb();
        BOOST_REQUIRE(decodeBds60(m.v, std::nullopt).has_value());
        m.at(x[0], 1, 0);
        BOOST_CHECK_MESSAGE(!decodeBds60(m.v, std::nullopt).has_value(), "6,0 status bit " << x[0]);
        m.at(x[1], x[2], 0);
        BOOST_CHECK_MESSAGE(decodeBds60(m.v, std::nullopt).has_value(), "6,0 cleared field at " << x[0]);
    }
}

BOOST_AUTO_TEST_CASE(bds60_inertial_rate_limit)
{
    Mb m;
    m.at(46, 1, 1).sat(47, 10, 187);  // 5984 ft/min
    BOOST_CHECK(decodeBds60(m.v, std::nullopt).has_value());
    m.sat(47, 10, 188);  // 6016
    BOOST_CHECK(!decodeBds60(m.v, std::nullopt).has_value());
    m.sat(47, 10, -188);
    BOOST_CHECK(!decodeBds60(m.v, std::nullopt).has_value());
}

BOOST_AUTO_TEST_CASE(infer_bds40_dropped_when_selected_altitude_out_of_range)
{
    // Passes 4,0 (MCP 51232 ft, FMS 0) and 5,0 (roll -39.4, track 90), not 6,0.
    Mb m;
    m.at(1, 1, 1).at(2, 12, 3202).at(14, 1, 1);
    BOOST_REQUIRE(decodeBds40(m.v).has_value());
    BOOST_REQUIRE(*decodeBds40(m.v)->sel_alt_mcp_ft > 50000);
    BOOST_REQUIRE(decodeBds50(m.v).has_value());
    BOOST_REQUIRE(!decodeBds60(m.v, std::nullopt).has_value());
    const auto r = inferCommb(m.v, CommbContext{});
    BOOST_CHECK(r.kind == CommbKind::bds50);
    BOOST_CHECK(r.b50.has_value() && !r.b40.has_value());
}

BOOST_AUTO_TEST_CASE(infer_bds40_survives_as_only_candidate)
{
    const Mb m = bds40Mb();
    BOOST_REQUIRE(decodeBds40(m.v).has_value());
    BOOST_REQUIRE(!decodeBds50(m.v).has_value());
    BOOST_REQUIRE(!decodeBds60(m.v, std::nullopt).has_value());
    CommbContext ctx;  // context that would contradict a 5,0 or 6,0 changes nothing
    ctx.track_deg = 10.0f;
    ctx.gs_kt = 100.0f;
    ctx.baro_rate_fpm = 3000;
    const auto r = inferCommb(m.v, ctx);
    BOOST_CHECK(r.kind == CommbKind::bds40);
    BOOST_CHECK(r.b40.has_value());
}

BOOST_AUTO_TEST_CASE(infer_passes_altitude_to_bds60)
{
    Mb m = bds60Mb();
    m.at(25, 10, 75);  // Mach 0.30 with IAS 280
    BOOST_REQUIRE(decodeBds60(m.v, std::nullopt).has_value());
    BOOST_CHECK(!decodeBds60(m.v, 35000).has_value());
    CommbContext ctx;
    ctx.alt_ft = 35000;
    const auto r = inferCommb(m.v, ctx);
    BOOST_CHECK(r.kind != CommbKind::bds60);
    BOOST_CHECK(!r.b60.has_value());
}

BOOST_AUTO_TEST_CASE(infer_empty)
{
    BOOST_CHECK(inferCommb(0, CommbContext{}).kind == CommbKind::empty);
}

BOOST_AUTO_TEST_CASE(infer_other_bds20)
{
    Mb m;
    m.at(1, 8, 0x20);
    const std::string cs = "KLM1023 ";
    for (unsigned i = 0; i < 8; ++i) {
        const char c = cs[i];
        m.at(9 + 6 * i, 6, c == ' ' ? 32u : (c >= 'A' && c <= 'Z') ? static_cast<unsigned>(c - 'A' + 1) : static_cast<unsigned>(c - '0' + 48));
    }
    BOOST_CHECK(inferCommb(m.v, CommbContext{}).kind == CommbKind::other);
    m.at(9, 6, 0);  // an invalid character
    BOOST_CHECK(inferCommb(m.v, CommbContext{}).kind == CommbKind::unknown);
    for (unsigned id : {0x10u, 0x17u, 0x30u}) {
        Mb o;
        o.at(1, 8, id).at(30, 8, 0x55);
        BOOST_CHECK_MESSAGE(inferCommb(o.v, CommbContext{}).kind == CommbKind::other, "BDS " << id);
    }
}

BOOST_AUTO_TEST_CASE(infer_unknown)
{
    Mb m;
    m.at(5, 1, 1);  // a value bit under a clear status in every register
    BOOST_CHECK(inferCommb(m.v, CommbContext{}).kind == CommbKind::unknown);
}

BOOST_AUTO_TEST_CASE(comm_b_frames_carry_the_mb)
{
    const Mb m = bds60Mb();
    const auto f = asFrame(testing::df20(0xABCDEF, testing::ac13Alt(35000), m.v));
    BOOST_CHECK_EQUAL(f.df, 20);
    BOOST_CHECK_EQUAL(f.field56(32), m.v);
    const auto g = asFrame(testing::df21(0xABCDEF, testing::id13Squawk("1200"), m.v, 2));
    BOOST_CHECK_EQUAL(g.df, 21);
    BOOST_CHECK_EQUAL(g.field56(32), m.v);
    BOOST_CHECK_EQUAL(flightStatus(g), 2);
    BOOST_CHECK(decodeBds60(g.field56(32), std::nullopt).has_value());
}

BOOST_AUTO_TEST_CASE(acas_ra_mv_fields)
{
    AcasRa in;
    in.ara = 0x2000;
    in.rac = 0x8;  // do not pass below
    in.tti = 1;
    in.threat_address = 0xABCDEF;
    const auto f = asFrame(testing::df16(0x123456, testing::ac13Alt(20000), testing::acasRaMv(in).v));
    const auto ra = decodeAcasRaMv(f);
    BOOST_REQUIRE(ra.has_value());
    BOOST_CHECK_EQUAL(ra->ara, 0x2000);
    BOOST_CHECK_EQUAL(ra->rac, 0x8);
    BOOST_CHECK(!ra->rat);
    BOOST_CHECK(!ra->mte);
    BOOST_CHECK_EQUAL(ra->tti, 1);
    BOOST_REQUIRE(ra->threat_address.has_value());
    BOOST_CHECK_EQUAL(*ra->threat_address, 0xABCDEFu);
    BOOST_CHECK(!ra->threat_alt_ft.has_value());
}

BOOST_AUTO_TEST_CASE(acas_ra_mv_tti2_threat_position)
{
    AcasRa in;
    in.ara = 0x3880;
    in.rac = 0x5;
    in.rat = false;
    in.mte = true;
    in.tti = 2;
    in.threat_alt_ft = 12300;
    in.threat_range_nm = 3.5f;
    in.threat_bearing_deg = 90.0f;
    const auto ra = decodeAcasRaMv(asFrame(testing::df16(0x123456, 0, testing::acasRaMv(in).v)));
    BOOST_REQUIRE(ra.has_value());
    BOOST_CHECK(ra->mte);
    BOOST_CHECK_EQUAL(ra->tti, 2);
    BOOST_REQUIRE(ra->threat_alt_ft.has_value());
    BOOST_CHECK_EQUAL(*ra->threat_alt_ft, 12300);
    BOOST_REQUIRE(ra->threat_range_nm.has_value());
    BOOST_CHECK_SMALL(*ra->threat_range_nm - 3.5f, 0.001f);
    BOOST_REQUIRE(ra->threat_bearing_deg.has_value());
    BOOST_CHECK_SMALL(*ra->threat_bearing_deg - 90.0f, 0.001f);
    BOOST_CHECK(!ra->threat_address.has_value());
}

BOOST_AUTO_TEST_CASE(acas_ra_text_pairwise)
{
    AcasRa ra;
    ra.ara = 0x3080;  // bit 1 + corrective + positive, upward
    BOOST_CHECK_EQUAL(acasRaText(ra), "climb");
    ra.ara = 0x3880;  // downward sense
    BOOST_CHECK_EQUAL(acasRaText(ra), "descend");
    ra.ara = 0x3000;  // corrective vertical speed limit
    BOOST_CHECK_EQUAL(acasRaText(ra), "level off");
    ra.ara = 0x2000;  // preventive vertical speed limit
    BOOST_CHECK_EQUAL(acasRaText(ra), "monitor vertical speed");
    ra.ara = 0x2080;  // preventive positive
    BOOST_CHECK_EQUAL(acasRaText(ra), "maintain vertical speed");
    ra.ara = 0x3080 | 0x0400 | 0x0200 | 0x0100;  // increase, reversal, crossing
    BOOST_CHECK_EQUAL(acasRaText(ra), "climb, increase, reversal, crossing");
    ra.rat = true;  // RAT wins whatever the ARA says
    BOOST_CHECK_EQUAL(acasRaText(ra), "clear of conflict");
    ra.ara = 0;
    BOOST_CHECK_EQUAL(acasRaText(ra), "clear of conflict");
}

BOOST_AUTO_TEST_CASE(acas_ra_text_multi_threat)
{
    AcasRa ra;
    ra.mte = true;
    ra.ara = 0x3080;  // bits 1, 2 and 7 are not read in this branch
    BOOST_CHECK_EQUAL(acasRaText(ra), "advisory");
    ra.ara = static_cast<uint16_t>((1u << (14 - 9)) | (1u << (14 - 12)));  // ARA bits 9 and 12
    BOOST_CHECK_EQUAL(acasRaText(ra), "climb, crossing");
    ra.ara = static_cast<uint16_t>((1u << 6) | (1u << 5) | (1u << 4) | (1u << 3) | (1u << 2) | (1u << 1) | 1u);
    BOOST_CHECK_EQUAL(acasRaText(ra),
                      "correct upward, climb, correct downward, descend, crossing, sense reversal");
    ra.ara = 1;  // bit 14 reserved
    BOOST_CHECK_EQUAL(acasRaText(ra), "advisory");
    ra.mte = false;
    ra.ara = static_cast<uint16_t>(1u << (14 - 11));  // bit 1 clear
    BOOST_CHECK_EQUAL(acasRaText(ra), "descend");
    ra.rat = true;
    BOOST_CHECK_EQUAL(acasRaText(ra), "clear of conflict");
}

BOOST_AUTO_TEST_CASE(acas_ra_tc28_same_decoder)
{
    AcasRa in;
    in.ara = 0x3880;
    in.rac = 0x3;
    in.rat = true;
    in.mte = true;
    in.tti = 1;
    in.threat_address = 0x4840D6;
    const auto ra = decodeAcasRaTc28(asFrame(testing::df17(0xABCDEF, testing::acasRaTc28(in))));
    BOOST_REQUIRE(ra.has_value());
    BOOST_CHECK_EQUAL(ra->ara, 0x3880);
    BOOST_CHECK_EQUAL(ra->rac, 0x3);
    BOOST_CHECK(ra->rat);
    BOOST_CHECK(ra->mte);
    BOOST_CHECK_EQUAL(*ra->threat_address, 0x4840D6u);
    // The same body in the two carriers decodes identically.
    const auto mv = decodeAcasRaMv(asFrame(testing::df16(0xABCDEF, 0, testing::acasRaMv(in).v)));
    BOOST_REQUIRE(mv.has_value());
    BOOST_CHECK_EQUAL(mv->ara, ra->ara);
    BOOST_CHECK_EQUAL(*mv->threat_address, *ra->threat_address);
    // Subtype 1 (emergency) and other type codes are not RA messages.
    testing::Me me = testing::acasRaTc28(in);
    me.set(5, 3, 1);
    BOOST_CHECK(!decodeAcasRaTc28(asFrame(testing::df17(0xABCDEF, me))).has_value());
    BOOST_CHECK(!decodeAcasRaTc28(asFrame(testing::df17(0xABCDEF, testing::typeOnly(19)))).has_value());
}

BOOST_AUTO_TEST_CASE(acas_common_sl_ri)
{
    const auto f = asFrame(testing::df16(0x123456, 0, 0, true, 5, 9));
    const auto c = decodeAcasCommon(f);
    BOOST_CHECK(c.vs_ground);
    BOOST_CHECK_EQUAL(c.sl, 5);
    BOOST_CHECK_EQUAL(c.ri, 9);
    const auto g = decodeAcasCommon(asFrame(testing::df16(0x123456, 0, 0, false, 7, 15)));
    BOOST_CHECK(!g.vs_ground);
    BOOST_CHECK_EQUAL(g.sl, 7);
    BOOST_CHECK_EQUAL(g.ri, 15);
    // DF0 (short): VS bit 5, SL bits 8..10, RI bits 13..16.
    testing::Bytes d0(7, 0);
    d0[0] = 0x04;
    d0[1] = static_cast<uint8_t>((3 << 5) | (6 >> 1));
    d0[2] = static_cast<uint8_t>((6 & 1) << 7);
    const auto h = decodeAcasCommon(asFrame(d0));
    BOOST_CHECK(h.vs_ground);
    BOOST_CHECK_EQUAL(h.sl, 3);
    BOOST_CHECK_EQUAL(h.ri, 6);
    // Not a DF0/DF16 frame: all zero.
    const auto o = decodeAcasCommon(asFrame(testing::df20(0x123456, 0, 0)));
    BOOST_CHECK(!o.vs_ground);
    BOOST_CHECK_EQUAL(o.sl, 0);
    BOOST_CHECK_EQUAL(o.ri, 0);
}

BOOST_AUTO_TEST_CASE(mv_without_vds30_is_nullopt)
{
    AcasRa in;
    in.ara = 0x3080;
    testing::Me mv = testing::acasRaMv(in);
    mv.set(0, 8, 0x20);  // a different VDS
    BOOST_CHECK(!decodeAcasRaMv(asFrame(testing::df16(0x123456, 0, mv.v))).has_value());
    // The right VDS in a DF20 frame is not an MV.
    BOOST_CHECK(!decodeAcasRaMv(asFrame(testing::df20(0x123456, 0, testing::acasRaMv(in).v))).has_value());
}
