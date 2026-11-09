#define BOOST_TEST_MODULE decode_uat
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "decode/uat_decoder.hpp"
#include "decode_test_support.hpp"
#include "uat_test_support.hpp"

using namespace skycued;
using namespace skycued::decode;
using skycued::testing::encodeUat;
using skycued::testing::toHex;
using skycued::testing::UatSpec;
using std::chrono::seconds;

namespace {

const auto t0 = std::chrono::system_clock::time_point{} + std::chrono::hours(100000);

uat::UatFrame frameOf(const UatSpec& s, std::optional<double> rssi = std::nullopt)
{
    uat::UatFrame f;
    f.bytes = encodeUat(s);
    f.rssi_dbfs = rssi;
    return f;
}

UatSpec airborne()
{
    UatSpec s;
    s.type = 1;
    s.lat = 52.1;
    s.lon = 4.6;
    s.alt_ft = 5000;
    s.nic = 8;
    s.ag = 0;
    s.ns_kt = 100;
    s.ew_kt = 100;
    s.call = "KLM123";
    s.emitter = 3;
    return s;
}

struct Rig : skycued::testing::DecoderRig<UatDecoder> {
    std::vector<TrackObservation>& out = obs;
    explicit Rig(DecoderConfig c = {}) : DecoderRig<UatDecoder>(c, "uat1") {}
    void feed(const UatSpec& s, double at_s = 0.0, std::optional<double> rssi = std::nullopt)
    {
        dec.feed(frameOf(s, rssi), t0 + std::chrono::milliseconds(static_cast<long>(at_s * 1000)));
    }
    const TrackObservation& last() const { return out.back(); }
};

DecoderConfig withRef()
{
    DecoderConfig c;
    c.reference = cpr::Position{52.0, 4.5};
    return c;
}

}  // namespace

BOOST_AUTO_TEST_CASE(direct_adsb_emits_position_and_fields)
{
    Rig r(withRef());
    r.feed(airborne());
    BOOST_REQUIRE_EQUAL(r.out.size(), 1u);
    const auto& o = r.last();
    BOOST_CHECK_EQUAL(o.id.address, 0xA66EF1u);
    BOOST_CHECK(o.id.type == skycue::AddressType::icao);
    BOOST_CHECK(o.source == skycue::Source::uat_978);
    BOOST_CHECK(!o.rebroadcast);
    BOOST_REQUIRE(o.lat && o.lon);
    BOOST_CHECK_SMALL(*o.lat - 52.1, 1e-5);
    BOOST_CHECK_SMALL(*o.lon - 4.6, 1e-5);
    BOOST_CHECK_EQUAL(o.alt_baro_ft.value_or(-1), 5000);
    BOOST_REQUIRE(o.gs_kt && o.track_deg);
    BOOST_CHECK_CLOSE(*o.gs_kt, 141.4f, 1.0);
    BOOST_CHECK_CLOSE(*o.track_deg, 45.0f, 1.0);
    BOOST_CHECK_EQUAL(o.nic.value_or(99), 8);
    BOOST_CHECK_EQUAL(o.callsign.value_or(""), "KLM123");
    BOOST_CHECK_EQUAL(o.input_id, "uat1");
    BOOST_CHECK_EQUAL(o.raw_line, "-" + [&] {
        std::string h = toHex(encodeUat(airborne()));
        for (auto& c : h) c = static_cast<char>(std::toupper(c));
        return h;
    }() + ";");
    BOOST_CHECK_EQUAL(r.dec.counters().decoded, 1u);
}

BOOST_AUTO_TEST_CASE(qualifier_rows_to_observations)
{
    struct Row { unsigned q; skycue::AddressType t; skycue::Source s; bool rb; };
    const Row rows[] = {
        {0, skycue::AddressType::icao, skycue::Source::uat_978, false},
        {1, skycue::AddressType::non_icao, skycue::Source::uat_978, false},
        {2, skycue::AddressType::icao, skycue::Source::tisb, true},
        {3, skycue::AddressType::tisb_trackfile, skycue::Source::tisb, true},
        {4, skycue::AddressType::non_icao, skycue::Source::uat_978, false},
        {5, skycue::AddressType::non_icao, skycue::Source::uat_978, false},
        {6, skycue::AddressType::non_icao, skycue::Source::tisb, true},
    };
    for (const auto& row : rows) {
        Rig r(withRef());
        auto s = airborne();
        s.qualifier = row.q;
        r.feed(s);
        BOOST_REQUIRE_MESSAGE(r.out.size() == 1u, "qualifier " << row.q);
        BOOST_CHECK(r.last().id.type == row.t);
        BOOST_CHECK(r.last().source == row.s);
        BOOST_CHECK_EQUAL(r.last().rebroadcast, row.rb);
    }
    Rig r(withRef());
    auto s = airborne();
    s.qualifier = 7;
    r.feed(s);
    BOOST_CHECK(r.out.empty());
    BOOST_CHECK_EQUAL(r.dec.counters().unknown_type, 1u);
    BOOST_CHECK_EQUAL(r.dec.keys(), 0u);
}

BOOST_AUTO_TEST_CASE(latlon_zero_nic5_is_a_position_but_fails_range_check)
{
    Rig r(withRef());
    UatSpec s = airborne();
    s.zero_position = true;
    s.nic = 5;
    s.alt_ft = 5000;
    r.feed(s);
    for (const auto& o : r.out) BOOST_CHECK(!o.lat && !o.lon);
    BOOST_CHECK_EQUAL(r.dec.counters().cpr_rejected, 1u);
    BOOST_CHECK_EQUAL(r.dec.counters().no_position, 1u);
    // State not poisoned: a good fix afterwards is accepted.
    r.feed(airborne(), 1.0);
    BOOST_REQUIRE(r.last().lat);
    BOOST_CHECK_SMALL(*r.last().lat - 52.1, 1e-5);
}

BOOST_AUTO_TEST_CASE(range_check_rejects_far_first_fix)
{
    Rig r(withRef());
    auto s = airborne();
    s.lat = 40.0;
    s.lon = -3.0;
    r.feed(s);
    BOOST_CHECK_EQUAL(r.dec.counters().cpr_rejected, 1u);
    for (const auto& o : r.out) BOOST_CHECK(!o.lat);
    // Without a reference the first fix anchors the range; a far second one is rejected.
    Rig n;
    n.feed(airborne());
    auto far = airborne();
    far.lat = 40.0;
    far.lon = -3.0;
    n.feed(far, 1.0);
    BOOST_CHECK_EQUAL(n.dec.counters().cpr_rejected, 1u);
    BOOST_REQUIRE_EQUAL(n.out.size(), 1u);
}

BOOST_AUTO_TEST_CASE(no_position_counted_not_emitted)
{
    Rig r(withRef());
    auto s = airborne();
    s.zero_position = true;
    s.nic = 0;
    r.feed(s);
    BOOST_CHECK_EQUAL(r.dec.counters().no_position, 1u);
    BOOST_CHECK_EQUAL(r.dec.counters().cpr_rejected, 0u);
    BOOST_REQUIRE_EQUAL(r.out.size(), 1u);  // the fields still merge and emit as a change
    BOOST_CHECK(!r.last().lat && !r.last().lon);
    BOOST_CHECK_EQUAL(r.last().alt_baro_ft.value_or(-1), 5000);
    BOOST_CHECK_EQUAL(r.last().callsign.value_or(""), "KLM123");
}

BOOST_AUTO_TEST_CASE(sv_baro_type_fills_alt_baro)
{
    Rig r;
    auto s = airborne();
    s.type = 0;
    r.feed(s);
    BOOST_CHECK_EQUAL(r.last().alt_baro_ft.value_or(-1), 5000);
    BOOST_CHECK(!r.last().alt_geom_ft);
}

BOOST_AUTO_TEST_CASE(sv_geometric_type_fills_alt_geom)
{
    Rig r;
    auto s = airborne();
    s.type = 0;
    s.alt_geometric = true;
    r.feed(s);
    BOOST_CHECK_EQUAL(r.last().alt_geom_ft.value_or(-1), 5000);
    BOOST_CHECK(!r.last().alt_baro_ft);
    BOOST_CHECK(!r.last().geom_minus_baro_ft);
}

BOOST_AUTO_TEST_CASE(aux_fills_the_other_altitude)
{
    Rig r;
    auto s = airborne();
    s.type = 1;
    s.aux_alt_ft = 5150;
    s.alt_geometric = false;
    r.feed(s, 3.0);
    const auto& o = r.last();
    BOOST_CHECK_EQUAL(o.alt_baro_ft.value_or(-1), 5000);
    BOOST_CHECK_EQUAL(o.alt_geom_ft.value_or(-1), 5150);
    BOOST_CHECK_EQUAL(o.geom_minus_baro_ft.value_or(-1), 150);
    BOOST_REQUIRE(o.alt_baro_time && o.alt_geom_time && o.geom_minus_baro_time);
    BOOST_CHECK(*o.alt_baro_time == o.received_time);
    BOOST_CHECK(*o.alt_geom_time == o.received_time);
    BOOST_CHECK(*o.geom_minus_baro_time == o.received_time);
}

BOOST_AUTO_TEST_CASE(aux_when_sv_is_geometric)
{
    Rig r;
    auto s = airborne();
    s.alt_geometric = true;
    s.alt_ft = 5150;
    s.aux_alt_ft = 5000;
    r.feed(s);
    BOOST_CHECK_EQUAL(r.last().alt_geom_ft.value_or(-1), 5150);
    BOOST_CHECK_EQUAL(r.last().alt_baro_ft.value_or(-1), 5000);
    BOOST_CHECK_EQUAL(r.last().geom_minus_baro_ft.value_or(-1), 150);
}

BOOST_AUTO_TEST_CASE(no_aux_no_difference)
{
    Rig r;
    auto s = airborne();
    s.type = 3;  // long frame without an AUX SV
    r.feed(s);
    BOOST_CHECK(r.last().alt_baro_ft);
    BOOST_CHECK(!r.last().alt_geom_ft);
    BOOST_CHECK(!r.last().geom_minus_baro_ft);
}

BOOST_AUTO_TEST_CASE(short_frame_one_altitude_only)
{
    Rig r;
    auto s = airborne();
    s.type = 0;
    s.aux_alt_ft = 5150;  // not encoded in a type 0 frame
    r.feed(s);
    BOOST_CHECK(r.last().alt_baro_ft);
    BOOST_CHECK(!r.last().alt_geom_ft);
    BOOST_CHECK(!r.last().geom_minus_baro_ft);
}

BOOST_AUTO_TEST_CASE(stale_partner_not_reused)
{
    Rig r;
    auto s = airborne();
    s.aux_alt_ft = 5150;
    r.feed(s, 0.0);
    auto b = airborne();
    b.type = 0;
    b.alt_ft = 5100;
    r.feed(b, 30.0);
    BOOST_REQUIRE_EQUAL(r.out.size(), 2u);
    BOOST_CHECK_EQUAL(r.last().alt_baro_ft.value_or(-1), 5100);
    BOOST_CHECK(!r.last().geom_minus_baro_ft);
}

BOOST_AUTO_TEST_CASE(trackfile_target_has_no_geom)
{
    Rig r;
    auto s = airborne();
    s.qualifier = 3;
    s.aux_alt_ft = 5150;
    r.feed(s);
    BOOST_CHECK_EQUAL(r.last().alt_baro_ft.value_or(-1), 5000);
    BOOST_CHECK(!r.last().alt_geom_ft);
    BOOST_CHECK(!r.last().geom_minus_baro_ft);
}

BOOST_AUTO_TEST_CASE(on_ground_suppresses_alt_baro)
{
    Rig r;
    auto s = airborne();
    s.ag = 2;
    s.ns_kt.reset();
    s.ew_kt.reset();
    s.ground_speed_kt = 12;
    s.track_type = 1;
    s.track_deg = 90.0;
    s.aux_alt_ft = 5150;
    r.feed(s);
    BOOST_CHECK(r.last().on_ground.value_or(false));
    BOOST_CHECK(!r.last().alt_baro_ft);
    BOOST_CHECK_EQUAL(r.last().alt_geom_ft.value_or(-1), 5150);
    BOOST_CHECK_EQUAL(r.last().gs_kt.value_or(-1), 12);
    // Airborne again: asserted false, baro back.
    auto a = airborne();
    r.feed(a, 1.0);
    BOOST_CHECK(!r.last().on_ground.value_or(true));
    BOOST_CHECK(r.last().alt_baro_ft);
}

BOOST_AUTO_TEST_CASE(ms_fields_callsign_category_squawk_emergency_nacp)
{
    Rig r;
    auto s = airborne();
    s.emitter = 1;  // A1
    s.call = "N123AB";
    s.emergency = 2;
    s.nacp = 9;
    r.feed(s);
    BOOST_CHECK_EQUAL(r.last().callsign.value_or(""), "N123AB");
    BOOST_CHECK_EQUAL(r.last().category.value_or(""), "A1");
    BOOST_CHECK_EQUAL(r.last().emergency.value_or(""), "medical");
    BOOST_CHECK_EQUAL(r.last().nac_p.value_or(99), 9);
    Rig q;
    auto t = airborne();
    t.csid = false;
    t.call = "7421";
    q.feed(t);
    BOOST_CHECK_EQUAL(q.last().squawk.value_or(""), "7421");
    BOOST_CHECK(!q.last().callsign);
}

BOOST_AUTO_TEST_CASE(unchanged_frame_emits_nothing)
{
    Rig r;
    auto s = airborne();
    s.zero_position = true;  // no position: only the field rule applies
    s.nic = 0;
    r.feed(s, 0.0);
    r.feed(s, 1.0);
    BOOST_CHECK_EQUAL(r.out.size(), 1u);
    BOOST_CHECK_EQUAL(r.dec.counters().observations, 1u);
    // A changed field without a new position emits without lat/lon.
    auto c = s;
    c.alt_ft = 5200;
    r.feed(c, 2.0);
    BOOST_REQUIRE_EQUAL(r.out.size(), 2u);
    BOOST_CHECK(!r.last().lat && !r.last().lon);
    BOOST_CHECK_EQUAL(r.last().alt_baro_ft.value_or(-1), 5200);
}

// Spec 7.2, as in the 1090 decoder: without a position, a change emits only after 10 s without
// an accepted one, at most once per second, and a held-back change waits for the next frame.
BOOST_AUTO_TEST_CASE(change_without_position_is_gated)
{
    Rig r(withRef());
    r.feed(airborne(), 0.0);  // an accepted position: emits
    BOOST_REQUIRE_EQUAL(r.out.size(), 1u);
    auto c = airborne();
    c.zero_position = true;
    c.nic = 0;
    c.alt_ft = 5200;
    r.feed(c, 5.0);  // changed, but a position was accepted 5 s ago
    BOOST_CHECK_EQUAL(r.out.size(), 1u);
    c.alt_ft = 5300;
    r.feed(c, 11.0);  // 11 s without a position: emits
    BOOST_REQUIRE_EQUAL(r.out.size(), 2u);
    BOOST_CHECK(!r.last().lat);
    BOOST_CHECK_EQUAL(r.last().alt_baro_ft.value_or(-1), 5300);
    c.alt_ft = 5400;
    r.feed(c, 11.5);  // within 1 s of the last change emission: held back
    BOOST_CHECK_EQUAL(r.out.size(), 2u);
    r.feed(c, 12.2);  // nothing new in the frame, but the held change goes out
    BOOST_REQUIRE_EQUAL(r.out.size(), 3u);
    BOOST_CHECK_EQUAL(r.last().alt_baro_ft.value_or(-1), 5400);
    r.feed(c, 13.5);
    BOOST_CHECK_EQUAL(r.out.size(), 3u);
}

BOOST_AUTO_TEST_CASE(vertical_rate_change_emits_and_a_flip_drops_the_other_source)
{
    Rig r;
    auto s = airborne();
    s.zero_position = true;
    s.nic = 0;
    s.vrate_fpm = 640;
    s.vrate_geometric = false;
    r.feed(s, 0.0);
    BOOST_REQUIRE_EQUAL(r.out.size(), 1u);
    BOOST_REQUIRE(r.last().baro_rate_fpm);
    BOOST_CHECK_EQUAL(*r.last().baro_rate_fpm, 640.0f);
    s.vrate_fpm = 704;  // only the rate changed
    r.feed(s, 2.0);
    BOOST_REQUIRE_EQUAL(r.out.size(), 2u);
    BOOST_CHECK_EQUAL(*r.last().baro_rate_fpm, 704.0f);
    s.vrate_geometric = true;  // the source flips: the baro rate is dropped at once
    s.vrate_fpm = 128;
    r.feed(s, 4.0);
    BOOST_REQUIRE_EQUAL(r.out.size(), 3u);
    BOOST_REQUIRE(r.last().geom_rate_fpm);
    BOOST_CHECK_EQUAL(*r.last().geom_rate_fpm, 128.0f);
    BOOST_CHECK(!r.last().baro_rate_fpm);
    s.vrate_geometric = false;
    s.vrate_fpm = 192;
    r.feed(s, 6.0);
    BOOST_REQUIRE_EQUAL(r.out.size(), 4u);
    BOOST_CHECK(!r.last().geom_rate_fpm);
    BOOST_REQUIRE(r.last().baro_rate_fpm);
}

BOOST_AUTO_TEST_CASE(rs_errors_are_summed)
{
    Rig r;
    auto f = frameOf(airborne());
    r.dec.feed(f, t0);
    BOOST_CHECK_EQUAL(r.dec.counters().rs_errors, 0u);
    f.rs_errors = 2;
    r.dec.feed(f, t0 + seconds(1));
    f.rs_errors = 1;
    r.dec.feed(f, t0 + seconds(2));
    BOOST_CHECK_EQUAL(r.dec.counters().rs_errors, 3u);
}

BOOST_AUTO_TEST_CASE(repeated_identical_fix_emits_position)
{
    Rig r(withRef());
    r.feed(airborne(), 0.0);
    r.feed(airborne(), 1.0);
    BOOST_REQUIRE_EQUAL(r.out.size(), 2u);
    BOOST_REQUIRE(r.last().lat && r.last().lon);
    BOOST_CHECK(*r.last().position_time == r.last().received_time);
    BOOST_CHECK(r.last().received_time > r.out[0].received_time);
}

BOOST_AUTO_TEST_CASE(identity_outlives_60s_with_the_key)
{
    Rig r;
    r.feed(airborne(), 0.0);
    auto b = airborne();
    b.type = 0;
    b.alt_ft = 5200;
    r.feed(b, 120.0);
    BOOST_REQUIRE_EQUAL(r.out.size(), 2u);
    BOOST_CHECK_EQUAL(r.last().callsign.value_or(""), "KLM123");
    BOOST_CHECK_EQUAL(r.last().category.value_or(""), "A3");
    BOOST_CHECK(r.last().emergency);
    BOOST_CHECK(!r.last().nac_p);  // dynamic: expired
    Rig q;
    auto t = airborne();
    t.csid = false;
    t.call = "7421";
    q.feed(t, 0.0);
    q.feed(b, 120.0);
    BOOST_CHECK_EQUAL(q.last().squawk.value_or(""), "7421");
}

BOOST_AUTO_TEST_CASE(fields_expire_after_60s)
{
    Rig r;
    auto s = airborne();
    s.aux_alt_ft = 5150;
    r.feed(s, 0.0);
    auto b = airborne();
    b.type = 0;
    b.alt_geometric = true;
    b.alt_ft = 5200;
    r.feed(b, 70.0);
    BOOST_REQUIRE_EQUAL(r.out.size(), 2u);
    BOOST_CHECK_EQUAL(r.last().alt_geom_ft.value_or(-1), 5200);
    BOOST_CHECK(!r.last().alt_baro_ft);  // 70 s old
    BOOST_CHECK_EQUAL(r.last().callsign.value_or(""), "KLM123");  // identity lives with the key
}

BOOST_AUTO_TEST_CASE(rebroadcast_flag_and_direct_never_share_a_key)
{
    Rig r(withRef());
    auto a = airborne();
    auto b = airborne();
    b.qualifier = 2;
    r.feed(a, 0.0);
    r.feed(b, 0.1);
    BOOST_CHECK_EQUAL(r.dec.keys(), 2u);
    BOOST_REQUIRE_EQUAL(r.out.size(), 2u);
    BOOST_CHECK(!r.out[0].rebroadcast);
    BOOST_CHECK(r.out[1].rebroadcast);
}

BOOST_AUTO_TEST_CASE(rssi_mean_of_last_8)
{
    Rig r;
    // Ten frames, the alt changes so each emits; the last eight are -10 dBFS x4 and -20 dBFS x4.
    const double lv[] = {-40, -40, -10, -10, -10, -10, -20, -20, -20, -20};
    for (int i = 0; i < 10; ++i) {
        auto s = airborne();
        s.alt_ft = 5000 + 100 * i;
        r.feed(s, i, lv[i]);
    }
    const double mean = (4 * 0.1 + 4 * 0.01) / 8.0;
    BOOST_REQUIRE(r.last().rssi);
    BOOST_CHECK_SMALL(*r.last().rssi - 10.0 * std::log10(mean), 0.06);
}

BOOST_AUTO_TEST_CASE(rssi_absent_without_key)
{
    Rig r;
    r.feed(airborne());
    BOOST_CHECK(!r.last().rssi);
}

BOOST_AUTO_TEST_CASE(eviction_idle_300s_and_max_keys)
{
    Rig r;
    auto a = airborne();
    r.feed(a, 0.0);
    auto b = airborne();
    b.address = 0x123456;
    r.feed(b, 1.0);
    BOOST_CHECK_EQUAL(r.dec.keys(), 2u);
    auto c = airborne();
    c.address = 0x222222;
    r.feed(c, 400.0);
    BOOST_CHECK_EQUAL(r.dec.keys(), 1u);
    BOOST_CHECK_EQUAL(r.dec.counters().evicted, 2u);

    DecoderConfig cfg;
    cfg.max_keys = 2;
    Rig m(cfg);
    for (uint32_t i = 0; i < 5; ++i) {
        auto s = airborne();
        s.address = 0x100000 + i;
        m.feed(s, i);
    }
    BOOST_CHECK_EQUAL(m.dec.keys(), 2u);
    BOOST_CHECK_EQUAL(m.dec.counters().evicted, 3u);
}

BOOST_AUTO_TEST_CASE(key_state_survives_other_keys_and_lru_touch)
{
    DecoderConfig cfg;
    cfg.max_keys = 3;
    Rig r(cfg);
    auto at = [](uint32_t addr, const char* call) {
        auto s = airborne();
        s.address = addr;
        s.call = call;
        return s;
    };
    r.feed(at(0x100001, "KEYA"), 0.0);
    r.feed(at(0x100002, "KEYB"), 1.0);
    r.feed(at(0x100003, "KEYC"), 2.0);
    BOOST_CHECK_EQUAL(r.dec.keys(), 3u);
    BOOST_CHECK_EQUAL(r.dec.counters().evicted, 0u);
    r.feed(at(0x100001, "KEYA"), 3.0);  // touch: A is the young end
    r.feed(at(0x100004, "KEYD"), 4.0);  // evicts B, not A
    BOOST_CHECK_EQUAL(r.dec.keys(), 3u);
    BOOST_CHECK_EQUAL(r.dec.counters().evicted, 1u);

    // A kept its callsign: a frame without one still reports KEYA.
    auto na = at(0x100001, "");
    r.feed(na, 5.0);
    BOOST_REQUIRE(r.last().callsign);
    BOOST_CHECK_EQUAL(*r.last().callsign, "KEYA");
    // B restarted as a new key: no callsign.
    auto nb = at(0x100002, "");
    r.feed(nb, 6.0);
    BOOST_CHECK(!r.last().callsign);

    Rig i;
    i.feed(at(0x100001, "KEYA"), 0.0);
    i.feed(at(0x100002, "KEYB"), 300.0);
    BOOST_CHECK_EQUAL(i.dec.keys(), 2u);
    Rig j;
    j.feed(at(0x100001, "KEYA"), 0.0);
    j.feed(at(0x100002, "KEYB"), 301.0);
    BOOST_CHECK_EQUAL(j.dec.keys(), 1u);
}

BOOST_AUTO_TEST_CASE(counters_add_up)
{
    Rig r(withRef());
    r.feed(airborne(), 0.0);  // decoded
    auto z = airborne();
    z.address = 0x111111;
    z.zero_position = true;
    z.nic = 0;
    r.feed(z, 1.0);  // no_position
    auto far = airborne();
    far.address = 0x222222;
    far.lat = 10.0;
    r.feed(far, 2.0);  // rejected, counted no_position
    auto q7 = airborne();
    q7.qualifier = 7;
    r.feed(q7, 3.0);  // unknown_type
    auto t8 = airborne();
    t8.type = 8;
    t8.address = 0x333333;
    r.feed(t8, 4.0);  // unmapped
    auto sh = airborne();
    sh.type = 0;
    sh.address = 0x444444;
    r.feed(sh, 5.0);  // decoded
    const auto& c = r.dec.counters();
    BOOST_CHECK_EQUAL(c.frames, 6u);
    BOOST_CHECK_EQUAL(c.frames, c.decoded + c.unknown_type + c.no_position + c.unmapped);
    BOOST_CHECK_EQUAL(c.decoded, 2u);
    BOOST_CHECK_EQUAL(c.unknown_type, 1u);
    BOOST_CHECK_EQUAL(c.no_position, 2u);
    BOOST_CHECK_EQUAL(c.unmapped, 1u);
}

BOOST_AUTO_TEST_CASE(type_7_to_10_and_ts_frames_unmapped)
{
    for (unsigned t = 7; t <= 10; ++t) {
        Rig r(withRef());
        auto s = airborne();
        s.type = t;
        r.feed(s);
        BOOST_CHECK_EQUAL(r.dec.counters().unmapped, 1u);
        BOOST_CHECK_EQUAL(r.dec.counters().decoded, 0u);
        BOOST_REQUIRE_EQUAL(r.out.size(), 1u);  // the state vector is still decoded and emitted
        BOOST_CHECK(r.last().lat);
        BOOST_CHECK(!r.last().callsign);  // no mode status in these types
    }
    Rig r;
    auto s = airborne();
    s.type = 11;  // no such payload: the length does not match
    r.feed(s);
    BOOST_CHECK_EQUAL(r.dec.counters().unknown_type, 1u);
    BOOST_CHECK(r.out.empty());
}

BOOST_AUTO_TEST_CASE(non_monotonic_arrival_does_not_crash)
{
    Rig r(withRef());
    r.feed(airborne(), 100.0);
    auto s = airborne();
    s.alt_ft = 5300;
    r.feed(s, 50.0);
    BOOST_REQUIRE_EQUAL(r.out.size(), 2u);
    BOOST_CHECK_EQUAL(r.last().alt_baro_ft.value_or(-1), 5300);
    BOOST_CHECK_EQUAL(r.dec.keys(), 1u);
}

BOOST_AUTO_TEST_CASE(fuzz_random_18_34_bytes_never_throws)
{
    std::mt19937 rng(0xC0FFEE);
    Rig r(withRef());
    for (int i = 0; i < 100000; ++i) {
        uat::UatFrame f;
        f.bytes.resize(rng() & 1 ? 18 : 34);
        for (auto& b : f.bytes) b = static_cast<uint8_t>(rng());
        if (rng() % 4 == 0) f.bytes[0] = static_cast<uint8_t>((f.bytes[0] & 7u) | ((rng() % 11) << 3));
        BOOST_REQUIRE_NO_THROW(r.dec.feed(f, t0 + std::chrono::milliseconds(i * 7)));
    }
    BOOST_CHECK_EQUAL(r.dec.counters().frames, 100000u);
    const auto& c = r.dec.counters();
    BOOST_CHECK_EQUAL(c.frames, c.decoded + c.unknown_type + c.no_position + c.unmapped);
    std::string chars = "0123456789abcdefABCDEF-;=. rssi\n";
    for (int i = 0; i < 100000; ++i) {
        std::string line;
        const std::size_t n = rng() % 80;
        for (std::size_t k = 0; k < n; ++k) line += chars[rng() % chars.size()];
        BOOST_REQUIRE_NO_THROW(uat::parseDownlinkLine(line));
    }
}

BOOST_AUTO_TEST_CASE(uat_heading_codes_map_to_heading_fields)
{
    for (unsigned t : {0u, 1u, 2u, 3u}) {
        Rig r;
        auto s = airborne();
        s.ag = 2;
        s.ns_kt.reset();
        s.ew_kt.reset();
        s.ground_speed_kt = 12;
        s.track_type = t;
        s.track_deg = 90.0;
        r.feed(s);
        BOOST_REQUIRE_EQUAL(r.out.size(), 1u);
        const auto& o = r.last();
        BOOST_CHECK_EQUAL(o.track_deg.has_value(), t == 1);
        BOOST_CHECK_EQUAL(o.heading_mag_deg.has_value(), t == 2);
        BOOST_CHECK_EQUAL(o.heading_true_deg.has_value(), t == 3);
        if (t == 2) BOOST_CHECK_CLOSE(*o.heading_mag_deg, 90.0f, 1.0);
        if (t == 3) BOOST_CHECK_CLOSE(*o.heading_true_deg, 90.0f, 1.0);
    }
}

BOOST_AUTO_TEST_CASE(rssi_exact_values_to_a_tenth_db)
{
    auto after = [](const std::vector<double>& levels) {
        Rig r;
        for (size_t i = 0; i < levels.size(); ++i) {
            auto s = airborne();
            s.alt_ft = 5000 + 100 * static_cast<int>(i);  // every frame changes and so emits
            r.feed(s, static_cast<double>(i), levels[i]);
        }
        BOOST_REQUIRE_EQUAL(r.out.size(), levels.size());
        BOOST_REQUIRE(r.last().rssi);
        return *r.last().rssi;
    };
    BOOST_CHECK_EQUAL(after({-40, -40, -10, -10, -10, -10, -20, -20, -20, -20}), -12.6);
    BOOST_CHECK_EQUAL(after(std::vector<double>(8, -3.0)), -3.0);
    BOOST_CHECK_EQUAL(after({-10, -20, -30}), -14.3);  // fewer than eight frames
    BOOST_CHECK_EQUAL(after({-10, -20, -30, -40, -50, -60, -70, -80, -90}), -28.6);  // the first fell out
}

BOOST_AUTO_TEST_CASE(rssi_non_finite_level_is_ignored)
{
    Rig r;
    for (int i = 0; i < 3; ++i) {
        auto s = airborne();
        s.alt_ft = 5000 + 100 * i;
        r.feed(s, i, -10.0);
    }
    BOOST_REQUIRE_EQUAL(r.out.size(), 3u);
    BOOST_REQUIRE(r.last().rssi);
    BOOST_CHECK_EQUAL(*r.last().rssi, -10.0);
    const double bad[] = {std::nan(""), std::numeric_limits<double>::infinity()};
    for (int k = 0; k < 2; ++k) {
        auto s = airborne();
        s.alt_ft = 5300 + 100 * k;
        r.feed(s, 3 + k, bad[k]);
        BOOST_REQUIRE_EQUAL(r.out.size(), static_cast<size_t>(4 + k));
        BOOST_REQUIRE(r.last().rssi);
        BOOST_CHECK_EQUAL(*r.last().rssi, -10.0);
    }
}
