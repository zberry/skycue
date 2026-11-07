#define BOOST_TEST_MODULE decode_fields
#include <boost/test/unit_test.hpp>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "coding/bits.hpp"
#include "decode/altitude.hpp"
#include "decode/frame.hpp"

using namespace skycued;
using namespace skycued::decode;

namespace {

// Worked examples of The 1090 MHz Riddle (Junzi Sun), ADS-B chapters:
// aircraft identification, airborne position, airborne velocity (messages
// A and B) and surface position.
const char* const kIdent = "8D4840D6202CC371C32CE0576098";
const char* const kVelA = "8D485020994409940838175B284F";
const char* const kVelB = "8DA05F219B06B6AF189400CBC33F";
const char* const kAirPos = "8D40621D58C382D690C8AC2863A7";
const char* const kSurface = "8C4841753AAB238733C8CD4020B1";

Frame frameFromHex(const std::string& hex)
{
    std::vector<uint8_t> b;
    BOOST_REQUIRE(coding::parseHex(hex, b));
    auto f = classify(b);
    BOOST_REQUIRE(f.has_value());
    return *f;
}

void setBits(std::vector<uint8_t>& b, unsigned first, unsigned count, uint32_t value)
{
    for (unsigned i = 0; i < count; ++i) {
        const unsigned pos = first + i;
        const bool on = (value >> (count - 1 - i)) & 1u;
        const uint8_t m = static_cast<uint8_t>(0x80u >> (pos % 8));
        if (on)
            b[pos / 8] |= m;
        else
            b[pos / 8] = static_cast<uint8_t>(b[pos / 8] & ~m);
    }
}

// A zeroed 14-byte DF17 frame with the given type code.
std::vector<uint8_t> esBytes(unsigned tc)
{
    std::vector<uint8_t> b(14, 0);
    setBits(b, 0, 5, 17);
    setBits(b, 32, 5, tc);
    return b;
}

Frame asFrame(const std::vector<uint8_t>& b)
{
    auto f = classify(b);
    BOOST_REQUIRE(f.has_value());
    return *f;
}

// Value bit index of each AC13/ID13 field (first transmitted bit is bit 12).
unsigned ac13Bit(const std::string& n)
{
    static const struct { const char* n; unsigned b; } tab[] = {
        {"C1", 12}, {"A1", 11}, {"C2", 10}, {"A2", 9}, {"C4", 8}, {"A4", 7}, {"M", 6},
        {"B1", 5}, {"Q", 4}, {"B2", 3}, {"D2", 2}, {"B4", 1}, {"D4", 0}};
    for (const auto& t : tab)
        if (n == t.n)
            return t.b;
    BOOST_FAIL("bad field name " + n);
    return 0;
}

uint32_t ac13Of(std::initializer_list<const char*> set)
{
    uint32_t v = 0;
    for (const char* s : set)
        v |= 1u << ac13Bit(s);
    return v;
}

// Q=1 code for N: the 11 bits of N around the M and Q bits.
uint32_t ac13FromN(uint32_t n)
{
    return ((n & 0x7E0u) << 2) | ((n & 0x10u) << 1) | 0x10u | (n & 0xFu);
}

}  // namespace

BOOST_AUTO_TEST_CASE(identification_klm1023)
{
    const auto id = decodeIdentification(frameFromHex(kIdent));
    BOOST_REQUIRE(id.has_value());
    BOOST_CHECK_EQUAL(id->callsign, "KLM1023");
    BOOST_CHECK_EQUAL(id->category, "A0");
    BOOST_CHECK_EQUAL(typeCode(frameFromHex(kIdent)), 4);
    BOOST_CHECK_EQUAL(decodeCallsignIdent(frameFromHex(kIdent)), "KLM1023");
}

BOOST_AUTO_TEST_CASE(callsign_invalid_char_drops_identification)
{
    auto b = esBytes(4);
    setBits(b, 37, 3, 3);
    // "AB" then a 0 code (not an allowed character).
    setBits(b, 40, 6, 1);
    setBits(b, 46, 6, 2);
    setBits(b, 52, 6, 0);
    for (unsigned i = 3; i < 8; ++i)
        setBits(b, 40 + 6 * i, 6, 32);  // spaces
    const Frame f = asFrame(b);
    BOOST_CHECK(!decodeIdentification(f).has_value());
    BOOST_CHECK_EQUAL(decodeCallsignIdent(f), "");

    setBits(b, 52, 6, 3);  // C: now valid, trailing spaces trimmed
    const auto id = decodeIdentification(asFrame(b));
    BOOST_REQUIRE(id.has_value());
    BOOST_CHECK_EQUAL(id->callsign, "ABC");
    BOOST_CHECK_EQUAL(id->category, "A3");

    auto c = b;
    setBits(c, 32, 5, 2);
    BOOST_CHECK_EQUAL(decodeIdentification(asFrame(c))->category, "C3");
    setBits(c, 32, 5, 5);
    BOOST_CHECK(!decodeIdentification(asFrame(c)).has_value());
}

BOOST_AUTO_TEST_CASE(velocity_subtype1)
{
    const auto v = decodeVelocity(frameFromHex(kVelA));
    BOOST_REQUIRE(v.has_value());
    BOOST_REQUIRE(v->gs_kt.has_value());
    BOOST_REQUIRE(v->track_deg.has_value());
    BOOST_CHECK_SMALL(*v->gs_kt - 159.0f, 1.0f);
    BOOST_CHECK_SMALL(*v->track_deg - 182.88f, 0.1f);
    BOOST_REQUIRE(v->rate_fpm.has_value());
    BOOST_CHECK_EQUAL(*v->rate_fpm, -832);
    BOOST_CHECK(v->rate_is_geom);  // source bit 0 = GNSS
    BOOST_REQUIRE(v->gnss_baro_diff_ft.has_value());
    BOOST_CHECK_EQUAL(*v->gnss_baro_diff_ft, 550);
}

BOOST_AUTO_TEST_CASE(velocity_subtype3_has_no_track)
{
    const auto v = decodeVelocity(frameFromHex(kVelB));
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK(!v->gs_kt.has_value());
    BOOST_CHECK(!v->track_deg.has_value());
    BOOST_REQUIRE(v->rate_fpm.has_value());
    BOOST_CHECK_EQUAL(*v->rate_fpm, -2304);
    BOOST_CHECK(!v->rate_is_geom);  // source bit 1 = barometric
    BOOST_CHECK(!v->gnss_baro_diff_ft.has_value());
}

BOOST_AUTO_TEST_CASE(velocity_supersonic_and_missing_fields)
{
    auto b = esBytes(19);
    setBits(b, 37, 3, 2);
    setBits(b, 45, 1, 0);
    setBits(b, 46, 10, 11);  // east 4 * 10 = 40 kt
    setBits(b, 56, 1, 0);
    setBits(b, 57, 10, 1);   // north 0 kt
    const auto v = decodeVelocity(asFrame(b));
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_SMALL(*v->gs_kt - 40.0f, 0.01f);
    BOOST_CHECK_SMALL(*v->track_deg - 90.0f, 0.01f);
    BOOST_CHECK(!v->rate_fpm.has_value());  // field is all zeros

    setBits(b, 46, 10, 0);  // component unavailable
    const auto w = decodeVelocity(asFrame(b));
    BOOST_REQUIRE(w.has_value());
    BOOST_CHECK(!w->gs_kt.has_value());
    BOOST_CHECK(!w->track_deg.has_value());

    setBits(b, 37, 3, 5);  // reserved subtype
    BOOST_CHECK(!decodeVelocity(asFrame(b)).has_value());
    BOOST_CHECK(!decodeVelocity(frameFromHex(kIdent)).has_value());
}

BOOST_AUTO_TEST_CASE(airborne_position_fields)
{
    const Frame f = frameFromHex(kAirPos);
    BOOST_CHECK_EQUAL(typeCode(f), 11);
    const auto cp = decodeCpr(f);
    BOOST_REQUIRE(cp.has_value());
    const CprFields& c = *cp;
    BOOST_CHECK(!c.odd);
    BOOST_CHECK(!c.surface);
    BOOST_CHECK_EQUAL(c.lat17, 93000u);
    BOOST_CHECK_EQUAL(c.lon17, 51372u);
    BOOST_CHECK(!decodeCpr(frameFromHex(kIdent)).has_value());
    BOOST_CHECK(!decodeCpr(frameFromHex(kVelA)).has_value());
    const auto alt = altitudeAc12(f.bits(40, 12));
    BOOST_REQUIRE(alt.has_value());
    BOOST_CHECK_EQUAL(*alt, 38000);
}

// TC 22 frame from tests/fixtures/opensky/n722bd_2026-11-01_florida/n722bd_position_data4.csv
// (rawmsg column): the 12-bit field is 605 m of GNSS height, OpenSky reports 605.028 m = 1985 ft.
// The barometric Q-bit rule would give 6525 ft.
BOOST_AUTO_TEST_CASE(tc22_altitude_is_gnss_height_in_metres)
{
    const Frame f = frameFromHex("8fa9ab17b625d5759cdbcb2423d4");
    BOOST_CHECK_EQUAL(typeCode(f), 22);
    BOOST_CHECK_EQUAL(f.bits(40, 12), 605u);
    const auto alt = altitudeGnssHeightM12(f.bits(40, 12));
    BOOST_REQUIRE(alt.has_value());
    BOOST_CHECK_EQUAL(*alt, 1985);
    BOOST_CHECK(!altitudeGnssHeightM12(0).has_value());
}

BOOST_AUTO_TEST_CASE(surface_position_fields)
{
    const Frame f = frameFromHex(kSurface);
    BOOST_CHECK_EQUAL(typeCode(f), 7);
    BOOST_CHECK_EQUAL(capability(f), 4);
    const auto cs = decodeCpr(f);
    BOOST_REQUIRE(cs.has_value());
    BOOST_CHECK(cs->surface);
    BOOST_CHECK(!cs->odd);
    const SurfaceMovement m = decodeSurfaceMovement(f);
    BOOST_REQUIRE(m.gs_kt.has_value());
    BOOST_CHECK_SMALL(*m.gs_kt - 18.0f, 0.01f);  // movement code 42
    BOOST_REQUIRE(m.track_deg.has_value());
    BOOST_CHECK_SMALL(*m.track_deg - 140.625f, 0.01f);  // 50 * 360 / 128

    // Movement table steps.
    auto b = esBytes(6);
    const struct { uint32_t code; float kt; } rows[] = {
        {1, 0.0f}, {2, 0.125f}, {8, 0.875f}, {9, 1.0f}, {12, 1.75f}, {13, 2.0f}, {38, 14.5f},
        {39, 15.0f}, {93, 69.0f}, {94, 70.0f}, {108, 98.0f}, {109, 100.0f}, {123, 170.0f}, {124, 175.0f}};
    for (const auto& r : rows) {
        setBits(b, 37, 7, r.code);
        const auto s = decodeSurfaceMovement(asFrame(b));
        BOOST_REQUIRE(s.gs_kt.has_value());
        BOOST_CHECK_SMALL(*s.gs_kt - r.kt, 0.0001f);
        BOOST_CHECK(!s.track_deg.has_value());  // status bit clear
    }
    for (uint32_t code : {0u, 125u, 127u}) {
        setBits(b, 37, 7, code);
        BOOST_CHECK(!decodeSurfaceMovement(asFrame(b)).gs_kt.has_value());
    }
    BOOST_CHECK(!decodeSurfaceMovement(frameFromHex(kAirPos)).gs_kt.has_value());
}

BOOST_AUTO_TEST_CASE(altitude_ac13_q_bit_and_gillham)
{
    // Q = 1: 25 ft steps from -1000 ft.
    BOOST_CHECK_EQUAL(*altitudeAc13(ac13FromN(0)), -1000);
    BOOST_CHECK_EQUAL(*altitudeAc13(ac13FromN(1)), -975);
    BOOST_CHECK_EQUAL(*altitudeAc13(ac13FromN(1560)), 38000);
    BOOST_CHECK_EQUAL(*altitudeAc13(ac13FromN(2047)), 50175);

    // Zero and the metric bit are unavailable.
    BOOST_CHECK(!altitudeAc13(0).has_value());
    BOOST_CHECK(!altitudeAc12(0).has_value());
    BOOST_CHECK(!altitudeAc13(ac13Of({"M"})).has_value());
    BOOST_CHECK(!altitudeAc13(ac13Of({"M", "Q", "B1"})).has_value());

    // Gillham: the lowest code is -1200 ft (C4 only); 0 ft is B2 B4 C2
    // (the 500 ft Gray code of step 2, and the third 100 ft state).
    BOOST_CHECK_EQUAL(*altitudeAc13(ac13Of({"C4"})), -1200);
    BOOST_CHECK_EQUAL(*altitudeAc13(ac13Of({"B2", "B4", "C2"})), 0);
    BOOST_CHECK_EQUAL(*altitudeAc13(ac13Of({"C2", "C4"})), -1100);
    BOOST_CHECK_EQUAL(*altitudeAc13(ac13Of({"C2"})), -1000);
    BOOST_CHECK_EQUAL(*altitudeAc13(ac13Of({"C1", "C2"})), -900);
    BOOST_CHECK_EQUAL(*altitudeAc13(ac13Of({"C1"})), -800);
    // Next 500 ft step: the 100 ft cycle reverses (Gray step 1 = B4).
    BOOST_CHECK_EQUAL(*altitudeAc13(ac13Of({"B4", "C1"})), -700);
    BOOST_CHECK_EQUAL(*altitudeAc13(ac13Of({"B4", "C4"})), -300);

    // AC12 shares the code with the M bit removed (Q at bit 8 of 12).
    BOOST_CHECK_EQUAL(*altitudeAc12(0x010u | 0x000u), -1000);
    BOOST_CHECK_EQUAL(*altitudeAc12(0x080u), -1200);  // C4 is the 5th bit of the 12
    BOOST_CHECK_EQUAL(*altitudeAc12(0x20Au), 0);      // C2 B2 B4

    // Illegal C patterns (C1 C2 C4 = 000, 101, 111) are rejected.
    BOOST_CHECK(!altitudeAc13(ac13Of({"A1"})).has_value());
    BOOST_CHECK(!altitudeAc13(ac13Of({"C1", "C4"})).has_value());
    BOOST_CHECK(!altitudeAc13(ac13Of({"C1", "C2", "C4"})).has_value());
}

BOOST_AUTO_TEST_CASE(altitude_gillham_is_a_gray_cycle)
{
    // Walk every code in order by altitude: neighbouring 100 ft steps differ
    // by exactly one of the eleven bits, and decoding returns the altitude.
    // Build the code from altitude with an independent encoder.
    uint32_t prev = 0;
    for (int alt = -1200; alt <= 126700; alt += 100) {
        const int k = (alt + 1300) / 100;  // 1-based count of 100 ft steps
        const int n = (k - 1) / 5;         // 500 ft step
        const int oh = (k - 1) % 5 + 1;    // position within it
        const int ohRaw = (n & 1) ? 6 - oh : oh;
        static const unsigned cOf[6] = {0, 1, 3, 2, 6, 4};  // C1 C2 C4 value
        const unsigned c = cOf[ohRaw];
        unsigned g = static_cast<unsigned>(n) ^ (static_cast<unsigned>(n) >> 1);
        uint32_t code = 0;
        code |= ((c >> 2) & 1u) << ac13Bit("C1");
        code |= ((c >> 1) & 1u) << ac13Bit("C2");
        code |= (c & 1u) << ac13Bit("C4");
        code |= ((g >> 7) & 1u) << ac13Bit("D2");
        code |= ((g >> 6) & 1u) << ac13Bit("D4");
        code |= ((g >> 5) & 1u) << ac13Bit("A1");
        code |= ((g >> 4) & 1u) << ac13Bit("A2");
        code |= ((g >> 3) & 1u) << ac13Bit("A4");
        code |= ((g >> 2) & 1u) << ac13Bit("B1");
        code |= ((g >> 1) & 1u) << ac13Bit("B2");
        code |= (g & 1u) << ac13Bit("B4");
        const auto got = altitudeAc13(code);
        BOOST_REQUIRE_MESSAGE(got.has_value(), "alt " << alt);
        BOOST_REQUIRE_EQUAL(*got, alt);
        if (alt > -1200) {
            const uint32_t d = code ^ prev;
            BOOST_REQUIRE_MESSAGE(d != 0 && (d & (d - 1)) == 0, "alt " << alt);
        }
        prev = code;
    }
}

BOOST_AUTO_TEST_CASE(squawk_id13)
{
    // 7700: A=7 B=7 C=0 D=0 -> A1 A2 A4 B1 B2 B4.
    const uint32_t id7700 = ac13Of({"A1", "A2", "A4", "B1", "B2", "B4"});
    BOOST_CHECK_EQUAL(id7700, 0xAAAu);
    BOOST_REQUIRE(squawkId13(id7700).has_value());
    BOOST_CHECK_EQUAL(*squawkId13(id7700), "7700");
    BOOST_CHECK_EQUAL(*squawkId13(0), "0000");
    // C and D digits: C1 D4 -> 0 0 1 4; D1 sits at bit 4 (where AC13 has Q).
    BOOST_CHECK_EQUAL(*squawkId13((1u << 12) | 1u), "0014");
    BOOST_CHECK_EQUAL(*squawkId13(1u << 4), "0001");
    BOOST_CHECK_EQUAL(*squawkId13(0x1FFFu & ~(1u << 6)), "7777");
    BOOST_CHECK(!squawkId13(0x2000u).has_value());
}

BOOST_AUTO_TEST_CASE(emergency_codes)
{
    const char* const names[] = {"none", "general", "medical", "minimum_fuel",
                                 "no_comms", "unlawful_interference", "downed_aircraft"};
    auto b = esBytes(28);
    setBits(b, 37, 3, 1);
    for (uint32_t e = 0; e < 7; ++e) {
        setBits(b, 40, 3, e);
        const auto r = decodeEmergency(asFrame(b));
        BOOST_REQUIRE(r.has_value());
        BOOST_CHECK_EQUAL(*r, names[e]);
    }
    setBits(b, 40, 3, 7);
    BOOST_CHECK(!decodeEmergency(asFrame(b)).has_value());

    setBits(b, 40, 3, 1);
    setBits(b, 43, 13, 0xAAAu);
    BOOST_CHECK_EQUAL(*decodeSquawkTc28(asFrame(b)), "7700");
    setBits(b, 37, 3, 2);  // other subtype
    BOOST_CHECK(!decodeEmergency(asFrame(b)).has_value());
    BOOST_CHECK(!decodeSquawkTc28(asFrame(b)).has_value());
    BOOST_CHECK(!decodeEmergency(frameFromHex(kIdent)).has_value());
}

BOOST_AUTO_TEST_CASE(operational_status_version_and_nac)
{
    auto b = esBytes(31);
    setBits(b, 37, 3, 0);
    setBits(b, 72, 3, 2);  // version 2
    setBits(b, 75, 1, 1);  // NIC supplement-A
    setBits(b, 76, 4, 9);  // NACp 9
    const auto o = decodeOperationalStatus(asFrame(b));
    BOOST_REQUIRE(o.has_value());
    BOOST_CHECK_EQUAL(o->version, 2);
    BOOST_CHECK(o->nic_a);
    BOOST_CHECK(!o->nic_c);
    BOOST_CHECK(!o->surface);
    BOOST_REQUIRE(o->nac_p.has_value());
    BOOST_CHECK_EQUAL(static_cast<int>(*o->nac_p), 9);

    setBits(b, 37, 3, 1);  // surface: NIC supplement-C is message bit 52 (1-based)
    setBits(b, 51, 1, 1);
    const auto s = decodeOperationalStatus(asFrame(b));
    BOOST_REQUIRE(s.has_value());
    BOOST_CHECK(s->surface);
    BOOST_CHECK(s->nic_c);

    setBits(b, 37, 3, 2);
    BOOST_CHECK(!decodeOperationalStatus(asFrame(b)).has_value());
    BOOST_CHECK(!decodeOperationalStatus(frameFromHex(kIdent)).has_value());
}

BOOST_AUTO_TEST_CASE(nic_tables_v2_airborne_and_surface)
{
    // Uncertainty chapter, NIC table for version 2.
    auto nic = [](int tc, int ver, bool a, bool bc, bool surf) {
        const auto r = nicFor(tc, ver, a, bc, surf);
        BOOST_REQUIRE(r.has_value());
        return static_cast<int>(*r);
    };
    BOOST_CHECK_EQUAL(nic(9, 2, false, false, false), 11);
    BOOST_CHECK_EQUAL(nic(10, 2, false, false, false), 10);
    BOOST_CHECK_EQUAL(nic(11, 2, true, true, false), 9);
    BOOST_CHECK_EQUAL(nic(11, 2, false, false, false), 8);
    BOOST_CHECK_EQUAL(nic(11, 2, true, false, false), 8);
    BOOST_CHECK_EQUAL(nic(12, 2, false, false, false), 7);
    BOOST_CHECK_EQUAL(nic(13, 2, false, true, false), 6);
    BOOST_CHECK_EQUAL(nic(14, 2, false, false, false), 5);
    BOOST_CHECK_EQUAL(nic(15, 2, false, false, false), 4);
    BOOST_CHECK_EQUAL(nic(16, 2, true, true, false), 3);
    BOOST_CHECK_EQUAL(nic(16, 2, false, false, false), 2);
    BOOST_CHECK_EQUAL(nic(17, 2, false, false, false), 1);
    BOOST_CHECK_EQUAL(nic(18, 2, false, false, false), 0);
    BOOST_CHECK_EQUAL(nic(20, 2, false, false, false), 11);
    BOOST_CHECK_EQUAL(nic(21, 2, false, false, false), 10);
    BOOST_CHECK_EQUAL(nic(22, 2, false, false, false), 0);

    BOOST_CHECK_EQUAL(nic(5, 2, false, false, true), 11);
    BOOST_CHECK_EQUAL(nic(6, 2, false, false, true), 10);
    BOOST_CHECK_EQUAL(nic(7, 2, true, false, true), 9);
    BOOST_CHECK_EQUAL(nic(7, 2, false, false, true), 8);
    BOOST_CHECK_EQUAL(nic(8, 2, true, true, true), 7);
    BOOST_CHECK_EQUAL(nic(8, 2, true, false, true), 6);
    BOOST_CHECK_EQUAL(nic(8, 2, false, true, true), 6);
    BOOST_CHECK_EQUAL(nic(8, 2, false, false, true), 0);

    // Version 1 uses the single supplement bit; version 0 has none.
    BOOST_CHECK_EQUAL(nic(11, 1, true, false, false), 9);
    BOOST_CHECK_EQUAL(nic(11, 0, true, true, false), 8);
    BOOST_CHECK_EQUAL(nic(8, 1, true, true, true), 0);

    // Type codes without a position, and airborne/surface mismatch.
    BOOST_CHECK(!nicFor(4, 2, false, false, false).has_value());
    BOOST_CHECK(!nicFor(19, 2, false, false, false).has_value());
    BOOST_CHECK(!nicFor(0, 2, false, false, false).has_value());
    BOOST_CHECK(!nicFor(9, 2, false, false, true).has_value());
    BOOST_CHECK(!nicFor(6, 2, false, false, false).has_value());
}

BOOST_AUTO_TEST_CASE(frame_length_df_mismatch_is_unknown_type)
{
    BOOST_CHECK(!classify(std::vector<uint8_t>(7, 0x8D)).has_value());   // DF17 in 7 bytes
    BOOST_CHECK(!classify(std::vector<uint8_t>(14, 0x20)).has_value());  // DF4 in 14 bytes
    BOOST_CHECK(!classify(std::vector<uint8_t>(14, 0xC0)).has_value());  // DF24
    BOOST_CHECK(!classify(std::vector<uint8_t>(7, 0x18)).has_value());   // DF3: not listed
    BOOST_CHECK(!classify(std::vector<uint8_t>(10, 0x8D)).has_value());
    BOOST_CHECK(!classify(std::vector<uint8_t>{}).has_value());

    const struct { uint8_t first; size_t len; FrameKind kind; int df; } ok[] = {
        {0x00, 7, FrameKind::df0, 0}, {0x20, 7, FrameKind::df4, 4}, {0x28, 7, FrameKind::df5, 5},
        {0x58, 7, FrameKind::df11, 11}, {0x80, 14, FrameKind::df16, 16}, {0x88, 14, FrameKind::df17, 17},
        {0x90, 14, FrameKind::df18, 18}, {0xA0, 14, FrameKind::df20, 20}, {0xA8, 14, FrameKind::df21, 21}};
    for (const auto& o : ok) {
        const auto f = classify(std::vector<uint8_t>(o.len, o.first));
        BOOST_REQUIRE(f.has_value());
        BOOST_CHECK(f->kind == o.kind);
        BOOST_CHECK_EQUAL(f->df, o.df);
    }
    const Frame f = frameFromHex(kIdent);
    BOOST_CHECK_EQUAL(f.bits(0, 8), 0x8Du);
    BOOST_CHECK_EQUAL(f.bits(4, 8), 0xD4u);
    BOOST_CHECK_THROW(f.bits(100, 13), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(capability_and_flight_status)
{
    BOOST_CHECK_EQUAL(capability(frameFromHex(kSurface)), 4);  // 0x8C: DF17, CA 4
    BOOST_CHECK_EQUAL(capability(frameFromHex(kIdent)), 5);    // 0x8D: CA 5

    // FS: 0 airborne, 1 on the ground, 2 alert airborne, 3 alert ground.
    for (int fs = 0; fs < 4; ++fs) {
        std::vector<uint8_t> b(7, 0);
        setBits(b, 0, 5, 4);
        setBits(b, 5, 3, static_cast<uint32_t>(fs));
        const Frame f = asFrame(b);
        BOOST_CHECK_EQUAL(flightStatus(f), fs);
        const bool ground = fs == 1 || fs == 3;
        BOOST_CHECK_EQUAL(ground, (flightStatus(f) & 1) != 0);
    }
    std::vector<uint8_t> d5(7, 0);
    setBits(d5, 0, 5, 5);
    setBits(d5, 5, 3, 3);
    BOOST_CHECK_EQUAL(flightStatus(asFrame(d5)), 3);
    BOOST_CHECK_EQUAL(flightStatus(frameFromHex(kIdent)), -1);

    std::vector<uint8_t> d0(7, 0);
    BOOST_CHECK(!verticalStatusGround(asFrame(d0)));
    setBits(d0, 5, 1, 1);
    BOOST_CHECK(verticalStatusGround(asFrame(d0)));
    std::vector<uint8_t> d4(7, 0);
    setBits(d4, 0, 5, 4);
    setBits(d4, 5, 1, 1);
    BOOST_CHECK(!verticalStatusGround(asFrame(d4)));
}
