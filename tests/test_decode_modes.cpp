// The 1090 decoder: address confirmation, CPR pairing and observation
// production (frame decoder design, sections 5 and 7).
#define BOOST_TEST_MODULE decode_modes
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cmath>
#include <optional>
#include <vector>

#include "coding/bits.hpp"
#include "decode/modes_decoder.hpp"
#include "decode_test_support.hpp"

using namespace skycued::testing;
using skycued::TrackObservation;
using skycued::decode::DecoderConfig;
using skycued::decode::ModesDecoder;
using skycue::AddressType;
using skycue::Source;

namespace {

namespace cpr = skycued::decode::cpr;
constexpr uint32_t kAddr = 0x4840D6;

ModesDecoder::Clock::time_point at(double s) {
    using namespace std::chrono;
    return ModesDecoder::Clock::time_point(duration_cast<ModesDecoder::Clock::duration>(duration<double>(1.7e9 + s)));
}

struct Rig : DecoderRig<ModesDecoder> {
    explicit Rig(DecoderConfig cfg = {}) : DecoderRig<ModesDecoder>(cfg, "in1") {}
    void feed(const Bytes& b, double t, std::optional<uint8_t> sig = std::nullopt) { dec.feed(b, at(t), sig); }
    const skycued::decode::DecodeCounters& c() const { return dec.counters(); }
};

std::string rawOf(const Bytes& b) { return "*" + skycued::coding::toHex(b, true) + ";"; }

const cpr::Position kP0{52.2572, 3.9193};

Bytes air(const cpr::Position& p, bool odd, int32_t alt = 38000, uint32_t addr = kAddr, int tc = 11) {
    return df17(addr, airbornePosition(tc, encodeCpr(p, odd, false), alt));
}
Bytes surf(const cpr::Position& p, bool odd, uint32_t addr = kAddr) {
    return df17(addr, surfacePosition(7, encodeCpr(p, odd, true), 40, 10));
}

}  // namespace

BOOST_AUTO_TEST_CASE(position_pair_emits_one_observation_with_fields) {
    Rig r;
    r.feed(df17(kAddr, identification(4, 3, "KLM1023")), 0.0);
    r.feed(df17(kAddr, velocity(100, 100, 640)), 0.2);
    const Bytes even = air(kP0, false), odd = air(kP0, true);
    r.feed(even, 1.0);
    BOOST_CHECK(r.obs.empty());
    r.feed(odd, 1.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    const auto& o = r.obs[0];
    const auto want = cpr::globalAirborne(encodeCpr(kP0, false, false), encodeCpr(kP0, true, false), true);
    BOOST_REQUIRE(want);
    BOOST_CHECK_SMALL(*o.lat - want->lat, 1e-5);
    BOOST_CHECK_SMALL(*o.lon - want->lon, 1e-5);
    BOOST_CHECK_EQUAL(*o.callsign, "KLM1023");
    BOOST_CHECK_EQUAL(*o.category, "A3");
    BOOST_CHECK_CLOSE(static_cast<double>(*o.gs_kt), 141.42, 0.1);
    BOOST_CHECK_CLOSE(static_cast<double>(*o.track_deg), 45.0, 0.1);
    BOOST_CHECK_EQUAL(*o.geom_rate_fpm, 640.0f);
    BOOST_CHECK(!o.baro_rate_fpm);
    BOOST_CHECK_EQUAL(*o.alt_baro_ft, 38000);
    BOOST_CHECK(o.source == Source::adsb_1090);
    BOOST_CHECK(o.id.type == AddressType::icao);
    BOOST_CHECK_EQUAL(o.id.address, kAddr);
    BOOST_CHECK_EQUAL(o.raw_line, rawOf(odd));
    BOOST_CHECK_EQUAL(o.input_id, "in1");
    BOOST_CHECK(o.on_ground && !*o.on_ground);
    BOOST_CHECK(o.position_time && *o.position_time == at(1.5));
    BOOST_CHECK(o.received_time == at(1.5));
    BOOST_CHECK_EQUAL(r.c().observations, 1u);
    BOOST_CHECK_EQUAL(r.c().frames, 4u);
    BOOST_CHECK_EQUAL(r.c().cpr_pending, 1u);
}

BOOST_AUTO_TEST_CASE(emitted_observation_common_fields_pin) {
    Rig r;
    r.feed(df17(kAddr, identification(4, 3, "KLM1023")), 0.0);
    r.feed(df17(kAddr, velocity(100, 100, 640)), 0.2);
    r.feed(df17(kAddr, operationalStatus(2, true, 9)), 0.4);
    r.feed(air(kP0, false, 38000, kAddr, 9), 1.0, 255);
    r.feed(air(kP0, true, 38000, kAddr, 9), 1.5, 255);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    const auto& o = r.obs[0];
    BOOST_CHECK_EQUAL(*o.callsign, "KLM1023");
    BOOST_CHECK_EQUAL(*o.category, "A3");
    BOOST_CHECK_CLOSE(static_cast<double>(*o.gs_kt), 141.42, 0.1);
    BOOST_CHECK_CLOSE(static_cast<double>(*o.track_deg), 45.0, 0.1);
    BOOST_CHECK_EQUAL(*o.geom_rate_fpm, 640.0f);
    BOOST_CHECK(!o.baro_rate_fpm);
    BOOST_REQUIRE(o.nic);
    BOOST_REQUIRE(o.nac_p);
    BOOST_CHECK_EQUAL(*o.nac_p, 9);
    BOOST_REQUIRE(o.rssi);
    BOOST_CHECK_SMALL(*o.rssi, 1e-9);
    BOOST_CHECK_EQUAL(o.input_id, "in1");
    BOOST_CHECK(!o.rebroadcast);
    // 61 s later the dynamic fields have expired; the identity stays with the key.
    r.feed(air(kP0, false, 38000, kAddr, 9), 62.5, 255);
    r.feed(air(kP0, true, 38000, kAddr, 9), 63.0, 255);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 2u);
    const auto& q = r.obs[1];
    BOOST_CHECK(!q.gs_kt && !q.track_deg && !q.geom_rate_fpm && !q.baro_rate_fpm);
    BOOST_CHECK(!q.nac_p);  // nic is renewed by the position frames themselves
    BOOST_CHECK_EQUAL(*q.callsign, "KLM1023");
    BOOST_CHECK_EQUAL(*q.category, "A3");
}

BOOST_AUTO_TEST_CASE(second_position_decodes_locally) {
    Rig r;
    r.feed(air(kP0, false), 1.0);
    r.feed(air(kP0, true), 1.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    // 13.5 s after the odd one: out of the pair window, local against the last position.
    const cpr::Position p2{kP0.lat + 0.01, kP0.lon + 0.01};
    r.feed(air(p2, false), 15.0);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 2u);
    BOOST_CHECK_SMALL(*r.obs[1].lat - p2.lat, 1e-4);
    BOOST_CHECK_SMALL(*r.obs[1].lon - p2.lon, 1e-4);
    BOOST_CHECK_EQUAL(r.c().cpr_pending, 1u);
    BOOST_CHECK_EQUAL(r.c().cpr_rejected, 0u);
}

BOOST_AUTO_TEST_CASE(pair_outside_window_stays_pending) {
    Rig r;
    r.feed(air(kP0, false), 0.0);
    r.feed(air(kP0, true), 11.0);
    BOOST_CHECK(r.obs.empty());
    BOOST_CHECK_EQUAL(r.c().cpr_pending, 2u);
    r.feed(air(kP0, false), 12.0);
    BOOST_CHECK_EQUAL(r.obs.size(), 1u);
}

BOOST_AUTO_TEST_CASE(nl_mismatch_pair_waits_for_a_new_half) {
    const cpr::Position a{10.46, 20.0}, b{10.48, 20.0};  // either side of an NL boundary
    {
        Rig r;
        r.feed(air(a, false), 0.0);
        r.feed(air(b, true), 0.5);
        BOOST_CHECK_EQUAL(r.c().cpr_pending, 2u);
        BOOST_CHECK_EQUAL(r.c().cpr_rejected, 0u);
        BOOST_CHECK(r.obs.empty());
        // The older (even) half was dropped: a lone even of the same NL as before does not pair.
        r.feed(air(a, false), 1.5);
        BOOST_CHECK_EQUAL(r.c().cpr_pending, 3u);
        BOOST_CHECK_EQUAL(r.c().cpr_rejected, 0u);
        BOOST_CHECK(r.obs.empty());
        // That even is now the stored one (the odd was dropped in turn): a matching odd pairs.
        r.feed(air(a, true), 2.0);
        BOOST_CHECK_EQUAL(r.obs.size(), 1u);
        BOOST_CHECK_EQUAL(r.c().cpr_rejected, 0u);
    }
    // The newer (odd) half is kept: a fresh even matching its NL produces a position.
    Rig r;
    r.feed(air(a, false), 0.0);
    r.feed(air(b, true), 0.5);
    r.feed(air(b, false), 1.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    BOOST_CHECK_SMALL(*r.obs[0].lat - b.lat, 1e-3);
    BOOST_CHECK_EQUAL(r.c().cpr_rejected, 0u);
}

BOOST_AUTO_TEST_CASE(speed_check_rejects_jump) {
    Rig r;
    r.feed(air(kP0, false), 0.0);
    r.feed(air(kP0, true), 0.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    const cpr::Position far{kP0.lat + 100.0 / 60.0, kP0.lon};
    r.feed(air(far, false), 5.0);
    BOOST_CHECK_EQUAL(r.c().cpr_rejected, 1u);
    BOOST_CHECK_EQUAL(r.obs.size(), 1u);
    // No local decode after a rejection, only a fresh global pair.
    r.feed(air(far, true), 5.4);
    BOOST_CHECK_EQUAL(r.c().cpr_rejected, 1u);
    BOOST_CHECK_EQUAL(r.obs.size(), 1u);
    r.feed(air(far, false), 5.8);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 2u);
    BOOST_CHECK_SMALL(*r.obs[1].lat - far.lat, 1e-4);
}

BOOST_AUTO_TEST_CASE(cpr_pairing_tolerates_backwards_time) {
    Rig r;
    r.feed(air(kP0, false), 10.0);
    r.feed(air(kP0, true), 9.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    BOOST_CHECK_EQUAL(r.c().cpr_rejected, 0u);
    // Windows count from the max-so-far time: 10.0 + 9.9 is still inside 10 s of 9.5's pair.
    r.feed(air(kP0, false), 19.9);
    BOOST_CHECK_EQUAL(r.obs.size(), 2u);
}

BOOST_AUTO_TEST_CASE(kind_change_discards_other_kind) {
    const cpr::Position gnd{52.31, 4.76};
    {
        // With a reference, a surface pair decodes to a surface position.
        DecoderConfig cfg;
        cfg.reference = cpr::Position{52.3, 4.7};
        Rig r(cfg);
        r.feed(surf(gnd, false), 1.0);
        r.feed(surf(gnd, true), 1.2);
        BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
        BOOST_CHECK_SMALL(*r.obs[0].lat - gnd.lat, 1e-3);
        BOOST_CHECK(r.obs[0].on_ground && *r.obs[0].on_ground);
    }
    // Without a reference or a last position nothing decodes: the airborne even half waits,
    // the surface frame discards it, and a later airborne odd half finds no partner.
    Rig r;
    r.feed(air(gnd, false), 0.0);
    r.feed(surf(gnd, false), 1.0);
    r.feed(air(gnd, true), 2.0);
    BOOST_CHECK(r.obs.empty());
    BOOST_CHECK_EQUAL(r.c().cpr_pending, 3u);
    BOOST_CHECK_EQUAL(r.c().cpr_rejected, 0u);
    // Control: without the surface frame in between, the same two halves pair.
    Rig q;
    q.feed(air(gnd, false), 0.0);
    q.feed(air(gnd, true), 2.0);
    BOOST_CHECK_EQUAL(q.obs.size(), 1u);
}

BOOST_AUTO_TEST_CASE(surface_needs_reference_or_last_position) {
    const cpr::Position gnd{52.31, 4.76};
    {
        Rig r;
        r.feed(surf(gnd, false), 0.0);
        r.feed(surf(gnd, true), 0.5);
        BOOST_CHECK_EQUAL(r.c().cpr_rejected, 1u);
        BOOST_CHECK(r.obs.empty());
    }
    DecoderConfig cfg;
    cfg.reference = cpr::Position{52.3, 4.7};
    Rig r(cfg);
    r.feed(surf(gnd, false), 0.0);
    r.feed(surf(gnd, true), 0.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    BOOST_CHECK_LT(cpr::distanceNm({*r.obs[0].lat, *r.obs[0].lon}, gnd), 1.0);
    BOOST_REQUIRE(r.obs[0].gs_kt);
    BOOST_CHECK_CLOSE(static_cast<double>(*r.obs[0].gs_kt), 16.0, 0.1);  // movement code 40
}

BOOST_AUTO_TEST_CASE(ap_reply_only_after_confirmation) {
    Rig r;
    r.feed(df4(kAddr, ac13Alt(5000)), 0.0);
    BOOST_CHECK_EQUAL(r.c().ap_unconfirmed, 1u);
    r.feed(df11(kAddr), 1.0);
    r.feed(df4(kAddr, ac13Alt(5000)), 2.0);
    BOOST_CHECK_EQUAL(r.c().ap_unconfirmed, 1u);
    BOOST_CHECK(r.obs.empty());
    const Bytes second = df4(kAddr, ac13Alt(6000));
    r.feed(second, 3.0);  // an altitude change emits
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    BOOST_CHECK_EQUAL(*r.obs[0].alt_baro_ft, 6000);
    BOOST_CHECK_EQUAL(r.obs[0].raw_line, rawOf(second));
    BOOST_CHECK(!r.obs[0].lat);
    r.feed(df4(kAddr, ac13Alt(6000)), 62.5);  // more than 60 s after the DF11
    BOOST_CHECK_EQUAL(r.c().ap_unconfirmed, 2u);
}

BOOST_AUTO_TEST_CASE(df11_iid_refreshes_only) {
    Rig r;
    r.feed(df11(kAddr, 5, 5), 0.0);
    BOOST_CHECK_EQUAL(r.c().crc_fail, 1u);
    BOOST_CHECK_EQUAL(r.dec.keys(), 0u);
    r.feed(df11(kAddr), 1.0);
    r.feed(df11(kAddr, 5, 5), 50.0);
    BOOST_CHECK_EQUAL(r.c().crc_fail, 1u);
    r.feed(df4(kAddr, ac13Alt(5000)), 100.0);  // 50 s after the refresh, 99 s after the DF11
    BOOST_CHECK_EQUAL(r.c().ap_unconfirmed, 0u);
    r.feed(df11(kAddr, 5, 200), 110.0);  // an IID above 79 is a failure
    BOOST_CHECK_EQUAL(r.c().crc_fail, 2u);
}

BOOST_AUTO_TEST_CASE(crc_fixed_frame_decodes) {
    Rig r;
    r.feed(air(kP0, false), 0.0);
    Bytes odd = air(kP0, true);
    const Bytes good = odd;
    odd[13] ^= 0x04;
    r.feed(odd, 0.5);
    BOOST_CHECK_EQUAL(r.c().crc_fixed, 1u);
    BOOST_CHECK_EQUAL(r.c().crc_fail, 0u);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    BOOST_CHECK_EQUAL(r.obs[0].raw_line, rawOf(good));
    // Two flipped bits are beyond repair.
    Bytes bad = air(kP0, true);
    bad[6] ^= 0x01;
    bad[9] ^= 0x10;
    r.feed(bad, 1.0);
    BOOST_CHECK_EQUAL(r.c().crc_fail, 1u);
}

BOOST_AUTO_TEST_CASE(ground_suppresses_altitude) {
    {
        // A stored AC13 altitude, then a surface position.
        DecoderConfig cfg;
        cfg.reference = cpr::Position{52.3, 4.7};
        Rig r(cfg);
        r.feed(df11(kAddr), 0.0);
        r.feed(df4(kAddr, ac13Alt(5000)), 1.0);
        r.feed(surf({52.31, 4.76}, false), 2.0);
        r.feed(surf({52.31, 4.76}, true), 2.5);
        BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
        BOOST_CHECK(!r.obs[0].alt_baro_ft);
        BOOST_CHECK(r.obs[0].on_ground && *r.obs[0].on_ground);
    }
    // CA 4 after an airborne position.
    Rig r;
    r.feed(df17(kAddr, identification(4, 3, "ABC1")), 0.0);
    r.feed(air(kP0, false, 5000), 1.0);
    r.feed(air(kP0, true, 5000), 1.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    BOOST_CHECK_EQUAL(*r.obs[0].alt_baro_ft, 5000);
    r.feed(df17(kAddr, identification(4, 3, "ABC2"), 4), 20.0);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 2u);
    BOOST_CHECK(!r.obs[1].alt_baro_ft);
    BOOST_CHECK(r.obs[1].on_ground && *r.obs[1].on_ground);
    BOOST_CHECK_EQUAL(*r.obs[1].callsign, "ABC2");
}

BOOST_AUTO_TEST_CASE(identity_change_emits_without_position) {
    Rig r;
    r.feed(df17(kAddr, identification(4, 3, "AAA1")), 0.0);
    BOOST_CHECK(r.obs.empty());  // the first value is not a change
    r.feed(df17(kAddr, identification(4, 3, "BBB2")), 20.0);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    BOOST_CHECK_EQUAL(*r.obs[0].callsign, "BBB2");
    BOOST_CHECK(!r.obs[0].lat && !r.obs[0].position_time);
    BOOST_CHECK(!r.obs[0].on_ground);  // a lone ident frame asserts nothing about the ground
    r.feed(df17(kAddr, identification(4, 3, "CCC3")), 20.5);
    BOOST_CHECK_EQUAL(r.obs.size(), 1u);  // rate limited to once per second
    r.feed(df17(kAddr, identification(4, 3, "CCC3")), 21.2);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 2u);
    BOOST_CHECK_EQUAL(*r.obs[1].callsign, "CCC3");
    r.feed(df17(kAddr, identification(4, 3, "CCC3")), 25.0);
    BOOST_CHECK_EQUAL(r.obs.size(), 2u);
    // A squawk change counts too.
    r.feed(df5(kAddr, id13Squawk("1200")), 26.0);  // unconfirmed address: ignored
    r.feed(df17(kAddr, emergencyStatus(0, "1200")), 27.0);
    r.feed(df17(kAddr, emergencyStatus(0, "7700")), 29.0);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 3u);
    BOOST_CHECK_EQUAL(*r.obs[2].squawk, "7700");
    BOOST_CHECK_EQUAL(*r.obs[2].emergency, "none");
}

BOOST_AUTO_TEST_CASE(emergency_is_identity_and_outlives_60s) {
    Rig r;
    r.feed(df17(kAddr, identification(4, 3, "AAA1")), 0.0);
    r.feed(df17(kAddr, emergencyStatus(1, "7700")), 1.0);
    r.feed(df17(kAddr, identification(4, 3, "BBB2")), 20.0);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_REQUIRE(r.obs.back().emergency);
    BOOST_CHECK_EQUAL(*r.obs.back().emergency, "general");
    const std::size_t n = r.obs.size();
    r.feed(df17(kAddr, identification(4, 3, "CCC3")), 200.0);  // 199 s later: still reported (spec 14 item 5)
    BOOST_REQUIRE_EQUAL(r.obs.size(), n + 1);
    BOOST_REQUIRE(r.obs.back().emergency);
    BOOST_CHECK_EQUAL(*r.obs.back().emergency, "general");
    // A change of the code is a change that emits.
    r.feed(df17(kAddr, emergencyStatus(0, "7700")), 210.0);
    BOOST_REQUIRE_EQUAL(r.obs.size(), n + 2);
    BOOST_CHECK_EQUAL(*r.obs.back().emergency, "none");
}

BOOST_AUTO_TEST_CASE(df18_cf_table) {
    struct Row {
        int cf;
        bool imf;
        AddressType type;
        Source src;
    };
    const Row rows[] = {{0, false, AddressType::icao, Source::adsb_1090},
                        {1, false, AddressType::non_icao, Source::adsb_1090},
                        {2, false, AddressType::icao, Source::tisb},
                        {2, true, AddressType::tisb_trackfile, Source::tisb},
                        {5, false, AddressType::non_icao, Source::tisb},
                        {6, false, AddressType::icao, Source::uat_978},
                        {6, true, AddressType::non_icao, Source::uat_978}};
    for (const Row& row : rows) {
        BOOST_TEST_CONTEXT("cf " << row.cf << " imf " << row.imf) {
            Rig r;
            auto mk = [&](bool odd) {
                return df18(row.cf, kAddr, airbornePosition(11, encodeCpr(kP0, odd, false), 20000, row.imf));
            };
            r.feed(mk(false), 0.0);
            r.feed(mk(true), 0.5);
            BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
            BOOST_CHECK(r.obs[0].id.type == row.type);
            BOOST_CHECK(r.obs[0].source == row.src);
            BOOST_CHECK_EQUAL(r.obs[0].id.address, kAddr);
        }
    }
    for (int cf : {3, 4, 7}) {
        Rig r;
        r.feed(df18(cf, kAddr, airbornePosition(11, encodeCpr(kP0, false, false), 20000)), 0.0);
        BOOST_CHECK_EQUAL(r.c().unknown_type, 1u);
        BOOST_CHECK_EQUAL(r.dec.keys(), 0u);
    }
}

BOOST_AUTO_TEST_CASE(direct_and_rebroadcast_never_pair) {
    Rig r;
    r.feed(air(kP0, false), 0.0);
    r.feed(df18(2, kAddr, airbornePosition(11, encodeCpr(kP0, true, false), 38000)), 0.5);
    BOOST_CHECK(r.obs.empty());
    BOOST_CHECK_EQUAL(r.c().cpr_pending, 2u);
    BOOST_CHECK_EQUAL(r.dec.keys(), 2u);
}

BOOST_AUTO_TEST_CASE(rssi_mean_of_last_eight) {
    auto run = [](const std::vector<int>& sigs) {
        Rig r;
        // The last two frames form the pair that emits; the rest are filler.
        double t = 0.0;
        for (size_t i = 0; i + 2 < sigs.size(); ++i, t += 0.05)
            r.feed(df17(kAddr, identification(4, 3, "RSSI1")), t, static_cast<uint8_t>(sigs[i]));
        r.feed(air(kP0, false), t, static_cast<uint8_t>(sigs[sigs.size() - 2]));
        r.feed(air(kP0, true), t + 0.05, static_cast<uint8_t>(sigs.back()));
        BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
        BOOST_REQUIRE(r.obs[0].rssi);
        return *r.obs[0].rssi;
    };
    BOOST_CHECK_SMALL(run(std::vector<int>(8, 255)), 0.05);
    BOOST_CHECK_CLOSE(run(std::vector<int>(8, 128)), -6.0, 2.0);  // within 0.12 dB
    BOOST_CHECK_SMALL(run(std::vector<int>(8, 128)) + 6.0, 0.1);
    BOOST_CHECK_SMALL(run({255, 0, 255, 0, 255, 0, 255, 0}) + 3.0, 0.1);
    // Older frames fall out of the ring.
    BOOST_CHECK_SMALL(run({0, 0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255}), 0.05);
    Rig r;
    r.feed(air(kP0, false), 0.0);
    r.feed(air(kP0, true), 0.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    BOOST_CHECK(!r.obs[0].rssi);
}

namespace {
// Filler identification frames, then the even/odd pair that emits; one signal byte per frame.
std::optional<double> rssiAfter(const std::vector<int>& sigs)
{
    Rig r;
    double t = 0.0;
    for (size_t i = 0; i + 2 < sigs.size(); ++i, t += 0.05)
        r.feed(df17(kAddr, identification(4, 3, "RSSI1")), t, static_cast<uint8_t>(sigs[i]));
    r.feed(air(kP0, false), t, static_cast<uint8_t>(sigs[sigs.size() - 2]));
    r.feed(air(kP0, true), t + 0.05, static_cast<uint8_t>(sigs.back()));
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    return r.obs[0].rssi;
}
}  // namespace

BOOST_AUTO_TEST_CASE(rssi_exact_values_to_a_tenth_db)
{
    BOOST_CHECK_EQUAL(*rssiAfter(std::vector<int>(8, 128)), -6.0);
    BOOST_CHECK_EQUAL(*rssiAfter({255, 128, 64, 32, 16, 8, 4, 2}), -7.8);
    BOOST_CHECK_EQUAL(*rssiAfter({10, 255, 128, 64, 32, 16, 8, 4, 2}), -7.8);  // the ring wrapped: the oldest fell out
    BOOST_CHECK_EQUAL(*rssiAfter({255, 0, 255, 0, 255, 0, 255, 0}), -3.0);
}

BOOST_AUTO_TEST_CASE(rssi_all_zero_signal_gives_no_rssi)
{
    BOOST_CHECK(!rssiAfter(std::vector<int>(8, 0)));
}

BOOST_AUTO_TEST_CASE(gnss_baro_difference_sets_geom_altitude) {
    Rig r;
    r.feed(air(kP0, false, 10000), 0.0);
    r.feed(air(kP0, true, 10000), 0.5);
    r.feed(df17(kAddr, velocity(100, 0, 0, true, 500)), 1.0);
    r.feed(air(kP0, false, 10000), 2.0);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 2u);
    BOOST_CHECK_EQUAL(*r.obs[1].alt_geom_ft, 10500);
    BOOST_CHECK_EQUAL(*r.obs[1].alt_baro_ft, 10000);
    // A TC20 altitude wins over a derived one.
    r.feed(air(kP0, true, 10600, kAddr, 20), 3.0);
    r.feed(df17(kAddr, velocity(100, 0, 0, true, 500)), 3.2);
    r.feed(air(kP0, false, 10000), 3.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 4u);
    BOOST_CHECK_EQUAL(*r.obs[3].alt_geom_ft, 10600);
    BOOST_REQUIRE(r.obs[3].baro_rate_fpm == std::nullopt);
}

// The TC19 difference and the measurement times of carried values (design 5.4, 7.1).
BOOST_AUTO_TEST_CASE(tc19_diff_sets_geom_minus_baro) {
    Rig r;
    r.feed(air(kP0, false, 10000), 0.0);
    r.feed(air(kP0, true, 10000), 0.5);
    r.feed(df17(kAddr, velocity(100, 0, 0, true, 300)), 1.0);
    r.feed(air(kP0, false, 10000), 2.0);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 2u);
    const auto& o = r.obs[1];
    BOOST_REQUIRE(o.geom_minus_baro_ft);
    BOOST_CHECK_EQUAL(*o.geom_minus_baro_ft, 300);
    BOOST_CHECK_EQUAL(*o.alt_geom_ft, 10300);
    BOOST_REQUIRE(o.alt_geom_time && o.alt_baro_time);
    BOOST_CHECK(*o.alt_geom_time == at(1.0));  // the TC19 arrival
    BOOST_CHECK(*o.alt_baro_time == at(2.0));  // the baro of this very frame
    // The first observation had no difference yet.
    BOOST_CHECK(!r.obs[0].geom_minus_baro_ft);
    BOOST_CHECK(!r.obs[0].alt_geom_time);
    BOOST_CHECK(*r.obs[0].alt_baro_time == at(0.5));
}

BOOST_AUTO_TEST_CASE(tc19_negative_diff) {
    Rig r;
    r.feed(air(kP0, false, 10000), 0.0);
    r.feed(air(kP0, true, 10000), 0.5);
    r.feed(df17(kAddr, velocity(100, 0, 0, true, -425)), 1.0);
    r.feed(air(kP0, false, 10000), 2.0);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 2u);
    BOOST_CHECK_EQUAL(*r.obs[1].geom_minus_baro_ft, -425);
    BOOST_CHECK_EQUAL(*r.obs[1].alt_geom_ft, 9575);
}

BOOST_AUTO_TEST_CASE(tc19_diff_without_baro_gives_diff_only_no_geom) {
    // The baro is 11.5 s old when the TC19 arrives: no geometric altitude is derived (5.4),
    // but the reported difference still goes out.
    Rig r;
    r.feed(air(kP0, false, 10000), 0.0);
    r.feed(air(kP0, true, 10000), 0.5);
    r.feed(df17(kAddr, velocity(100, 0, 0, true, 300)), 12.0);
    r.feed(air(kP0, false, 10000), 12.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 2u);
    BOOST_REQUIRE(r.obs[1].geom_minus_baro_ft);
    BOOST_CHECK_EQUAL(*r.obs[1].geom_minus_baro_ft, 300);
    BOOST_CHECK(!r.obs[1].alt_geom_ft);
    BOOST_CHECK(!r.obs[1].alt_geom_time);
}

BOOST_AUTO_TEST_CASE(tc2022_sets_alt_geom_time_no_diff) {
    Rig r;
    r.feed(air(kP0, false, 10000), 0.0);
    r.feed(air(kP0, true, 10000), 0.5);
    r.feed(air(kP0, true, 10600, kAddr, 20), 3.0);
    r.feed(air(kP0, false, 10000), 3.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 3u);  // the TC20 frame emits too
    const auto& o = r.obs[2];
    BOOST_CHECK_EQUAL(*o.alt_geom_ft, 10600);
    BOOST_REQUIRE(o.alt_geom_time);
    BOOST_CHECK(*o.alt_geom_time == at(3.0));
    BOOST_CHECK(!o.geom_minus_baro_ft);
}

BOOST_AUTO_TEST_CASE(tc19_then_tc20_15s_later_emits_no_stale_diff) {
    Rig r;
    r.feed(air(kP0, false, 10000), 0.0);
    r.feed(air(kP0, true, 10000), 0.5);
    r.feed(df17(kAddr, velocity(100, 0, 0, true, 300)), 1.0);
    r.feed(air(kP0, true, 10600, kAddr, 20), 4.0);
    r.feed(air(kP0, false, 10000), 16.0);  // 15 s after the TC19
    const auto& o = r.obs.back();
    BOOST_CHECK_EQUAL(*o.alt_geom_ft, 10600);
    BOOST_CHECK(!o.geom_minus_baro_ft);
}

BOOST_AUTO_TEST_CASE(newer_tc19_with_stale_baro_keeps_times_consistent) {
    Rig r;
    r.feed(air(kP0, false, 10000), 0.0);
    r.feed(air(kP0, true, 10000), 0.5);
    r.feed(df17(kAddr, velocity(100, 0, 0, true, 300)), 1.0);   // derived at 1.0
    r.feed(df17(kAddr, velocity(100, 0, 0, true, 350)), 12.0);  // baro 11.5 s old: no re-derive
    r.feed(air(kP0, false, 10000), 12.5);
    const auto& o = r.obs.back();
    // The newer difference supersedes the derived value: it is emitted alone, with its own time.
    BOOST_REQUIRE(o.geom_minus_baro_ft && o.geom_minus_baro_time);
    BOOST_CHECK_EQUAL(*o.geom_minus_baro_ft, 350);
    BOOST_CHECK(*o.geom_minus_baro_time == at(12.0));
    BOOST_CHECK(!o.alt_geom_ft);
    BOOST_CHECK(!o.alt_geom_time);
}

BOOST_AUTO_TEST_CASE(lone_tc19_diff_keeps_its_time) {
    Rig r;
    r.feed(air(kP0, false, 10000), 0.0);
    r.feed(air(kP0, true, 10000), 0.5);
    r.feed(df17(kAddr, velocity(100, 0, 0, true, 300)), 1.0);
    // 20 s on, a new identity emits an observation that carries the difference.
    r.feed(df17(kAddr, identification(4, 3, "TEST1")), 20.9);
    r.feed(df17(kAddr, identification(4, 3, "TEST2")), 21.0);
    const auto& o = r.obs.back();
    BOOST_REQUIRE(o.geom_minus_baro_ft && o.geom_minus_baro_time);
    BOOST_CHECK_EQUAL(*o.geom_minus_baro_ft, 300);
    BOOST_CHECK(*o.geom_minus_baro_time == at(1.0));
    BOOST_CHECK(o.received_time - *o.geom_minus_baro_time >= std::chrono::seconds(19));
}

BOOST_AUTO_TEST_CASE(carried_value_keeps_its_own_time) {
    Rig r;
    r.feed(air(kP0, false, 10000), 0.0);
    r.feed(air(kP0, true, 10000), 0.5);
    r.feed(df17(kAddr, velocity(100, 0, 0, true, 300)), 1.0);
    // 40 s on, a new identity (no altitude in it) emits an observation that carries the old values.
    r.feed(df17(kAddr, identification(4, 3, "TEST1")), 40.9);
    r.feed(df17(kAddr, identification(4, 3, "TEST2")), 41.0);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 2u);
    const auto& o = r.obs.back();
    BOOST_REQUIRE(o.alt_baro_ft && o.alt_baro_time);
    BOOST_CHECK(*o.alt_baro_time == at(0.5));
    BOOST_CHECK(*o.alt_baro_time < o.received_time);
    BOOST_REQUIRE(o.alt_geom_ft && o.alt_geom_time);
    BOOST_CHECK(*o.alt_geom_time == at(1.0));
}

BOOST_AUTO_TEST_CASE(tc0_counts_no_position) {
    Rig r;
    r.feed(df17(kAddr, typeOnly(0)), 0.0);
    BOOST_CHECK_EQUAL(r.c().no_position, 1u);
    BOOST_CHECK_EQUAL(r.c().decoded, 0u);
}

BOOST_AUTO_TEST_CASE(tc29_counts_unmapped) {
    Rig r;
    r.feed(df17(kAddr, typeOnly(29)), 0.0);
    BOOST_CHECK_EQUAL(r.c().unmapped, 1u);
}

BOOST_AUTO_TEST_CASE(tc23_counts_unknown_type) {
    Rig r;
    r.feed(df17(kAddr, typeOnly(23)), 0.0);
    BOOST_CHECK_EQUAL(r.c().unknown_type, 1u);
    r.feed(Bytes{0x00, 0x01}, 1.0);  // bad length
    BOOST_CHECK_EQUAL(r.c().unknown_type, 2u);
    r.feed(Bytes(7, 0xFF), 2.0);  // DF 31
    BOOST_CHECK_EQUAL(r.c().unknown_type, 3u);
}

BOOST_AUTO_TEST_CASE(operational_status_feeds_nic_and_nac) {
    Rig r;
    r.feed(df17(kAddr, operationalStatus(2, true, 9)), 0.0);
    r.feed(air(kP0, false, 20000, kAddr, 9), 1.0);
    r.feed(air(kP0, true, 20000, kAddr, 9), 1.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    BOOST_REQUIRE(r.obs[0].nic);
    BOOST_REQUIRE(r.obs[0].nac_p);
    BOOST_CHECK_EQUAL(*r.obs[0].nac_p, 9);
}

BOOST_AUTO_TEST_CASE(keys_idle_and_full_eviction) {
    {
        Rig r;
        r.feed(df11(0x111111), 0.0);
        r.feed(df11(0x222222), 400.0);
        BOOST_CHECK_EQUAL(r.c().evicted, 1u);
        BOOST_CHECK_EQUAL(r.dec.keys(), 1u);
    }
    DecoderConfig cfg;
    cfg.max_keys = 2;
    Rig r(cfg);
    r.feed(df11(0x111111), 0.0);
    r.feed(df11(0x222222), 1.0);
    r.feed(df11(0x333333), 2.0);
    BOOST_CHECK_EQUAL(r.c().evicted, 1u);
    BOOST_CHECK_EQUAL(r.dec.keys(), 2u);
}

BOOST_AUTO_TEST_CASE(key_state_survives_other_keys_and_lru_touch) {
    DecoderConfig cfg;
    cfg.max_keys = 3;
    Rig r(cfg);
    const uint32_t a = kAddr, b = kAddr + 1, c = kAddr + 2, d = kAddr + 3;
    r.feed(df17(a, identification(4, 3, "KEYA")), 0.0);
    r.feed(df17(b, identification(4, 3, "KEYB")), 1.0);
    r.feed(df17(c, identification(4, 3, "KEYC")), 2.0);
    BOOST_CHECK_EQUAL(r.dec.keys(), 3u);
    BOOST_CHECK_EQUAL(r.c().evicted, 0u);
    r.feed(df17(a, identification(4, 3, "KEYA")), 3.0);  // touch: A is the young end
    r.feed(df17(d, identification(4, 3, "KEYD")), 4.0);  // evicts B, not A
    BOOST_CHECK_EQUAL(r.dec.keys(), 3u);
    BOOST_CHECK_EQUAL(r.c().evicted, 1u);

    r.obs.clear();
    r.feed(air(kP0, false, 38000, a), 5.0);
    r.feed(air(kP0, true, 38000, a), 5.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    BOOST_REQUIRE(r.obs[0].callsign);
    BOOST_CHECK_EQUAL(*r.obs[0].callsign, "KEYA");

    r.obs.clear();
    r.feed(air(kP0, false, 38000, b), 6.0);
    r.feed(air(kP0, true, 38000, b), 6.5);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    BOOST_CHECK(!r.obs[0].callsign);  // B restarted as a new key
}

BOOST_AUTO_TEST_CASE(idle_eviction_boundary_is_300_s) {
    {
        Rig r;
        r.feed(df11(0x111111), 0.0);
        r.feed(df11(0x222222), 300.0);
        BOOST_CHECK_EQUAL(r.dec.keys(), 2u);
        BOOST_CHECK_EQUAL(r.c().evicted, 0u);
    }
    Rig r;
    r.feed(df11(0x111111), 0.0);
    r.feed(df11(0x222222), 301.0);
    BOOST_CHECK_EQUAL(r.dec.keys(), 1u);
    BOOST_CHECK_EQUAL(r.c().evicted, 1u);
}

BOOST_AUTO_TEST_CASE(decoder_marks_rebroadcast_class) {
    for (int cf : {2, 5, 6}) {
        Rig r;
        r.feed(df18(cf, kAddr, airbornePosition(11, encodeCpr(kP0, false, false), 20000)), 0.0);
        r.feed(df18(cf, kAddr, airbornePosition(11, encodeCpr(kP0, true, false), 20000)), 0.5);
        BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
        BOOST_CHECK(r.obs[0].rebroadcast);
    }
    Rig d;
    d.feed(air(kP0, false), 0.0);
    d.feed(air(kP0, true), 0.5);
    BOOST_REQUIRE_EQUAL(d.obs.size(), 1u);
    BOOST_CHECK(!d.obs[0].rebroadcast);
}

// ---- Phase 6: Comm-B, ACAS RA, TC29 and airspeed (spec section 14 item 8) ----

namespace {

using skycued::testing::Mb;
using skycued::kNavAltHold;
using skycued::kNavAutopilot;
using skycued::kNavLnav;
using skycued::kNavTcas;
using skycued::kNavVnav;

// Hand-built registers; Mb bit numbers are the amendment's (1-based).
Mb bds40Mb() {  // MCP 36000 ft, FMS 35008 ft, QNH 1013.2, VNAV + ALTHOLD
    Mb m;
    m.at(1, 1, 1).at(2, 12, 36000 / 16);
    m.at(14, 1, 1).at(15, 12, 35008 / 16);
    m.at(27, 1, 1).at(28, 12, 2132);
    m.at(48, 1, 1).at(49, 1, 1).at(50, 1, 1);
    return m;
}
Mb bds50Mb() {  // roll -12.3, track 271.4, GS 452, rate -0.5 deg/s, TAS 470
    Mb m;
    m.at(1, 1, 1).sat(2, 10, -70);
    m.at(12, 1, 1).at(13, 11, 1544);
    m.at(24, 1, 1).at(25, 10, 226);
    m.at(35, 1, 1).sat(36, 10, -16);
    m.at(46, 1, 1).at(47, 10, 235);
    return m;
}
Mb bds60Mb() {  // heading 95.6, IAS 280, Mach 0.78, baro -1216, inertial -1184
    Mb m;
    m.at(1, 1, 1).at(2, 11, 544);
    m.at(13, 1, 1).at(14, 10, 280);
    m.at(24, 1, 1).at(25, 10, 195);
    m.at(35, 1, 1).sat(36, 10, -38);
    m.at(46, 1, 1).sat(47, 10, -37);
    return m;
}
// Passes both the 5,0 and the 6,0 checks (track 225, GS 200, TAS 100 / IAS 256, Mach 0.4,
// rates 0 and 1600); fails 4,0. The baro rate bits are set by the caller.
Mb ambiguousMb(int baro_rate_steps = 0) {
    Mb m;
    m.at(1, 1, 1).at(12, 1, 1).at(13, 1, 1).at(14, 10, 256);
    m.at(24, 1, 1).at(25, 10, 100);
    m.at(35, 1, 1).sat(36, 10, baro_rate_steps);
    m.at(46, 1, 1).at(47, 10, 50);
    return m;
}

// A confirmed key with a known barometric altitude; the second altitude differs so the
// following Comm-B reply (carrying alt2) is a change and emits.
void confirmedWithAlt(Rig& r, int32_t alt1, double t = 0.0) {
    r.feed(df17(kAddr, identification(4, 3, "KLM1023")), t);
    r.feed(df4(kAddr, ac13Alt(alt1)), t + 0.1);
}

skycued::decode::AcasRa raOf(uint16_t ara, bool rat = false, uint8_t rac = 0) {
    skycued::decode::AcasRa ra;
    ra.ara = ara;
    ra.rat = rat;
    ra.rac = rac;
    return ra;
}

}  // namespace

BOOST_AUTO_TEST_CASE(commb_bds60_sets_heading_ias_rates) {
    Rig r;
    confirmedWithAlt(r, 35000);
    r.feed(df20(kAddr, ac13Alt(35025), bds60Mb().v), 1.0);
    BOOST_CHECK_EQUAL(r.c().commb_bds60, 1u);
    BOOST_REQUIRE(!r.obs.empty());
    const auto& o = r.obs.back();
    BOOST_REQUIRE(o.heading_mag_deg && o.ias_kt && o.mach && o.baro_rate_fpm && o.geom_rate_fpm);
    BOOST_CHECK_CLOSE(static_cast<double>(*o.heading_mag_deg), 95.6, 0.1);
    BOOST_CHECK_EQUAL(*o.ias_kt, 280);
    BOOST_CHECK_CLOSE(static_cast<double>(*o.mach), 0.78, 0.5);
    BOOST_CHECK_EQUAL(*o.baro_rate_fpm, -1216.0f);
    BOOST_CHECK_EQUAL(*o.geom_rate_fpm, -1184.0f);
}

BOOST_AUTO_TEST_CASE(commb_bds40_sets_intent) {
    Rig r;
    confirmedWithAlt(r, 35000);
    r.feed(df20(kAddr, ac13Alt(35025), bds40Mb().v), 1.0);
    BOOST_CHECK_EQUAL(r.c().commb_bds40, 1u);
    BOOST_REQUIRE(!r.obs.empty());
    const auto& o = r.obs.back();
    BOOST_REQUIRE(o.sel_alt_ft && o.sel_alt_fms_ft && o.qnh_mb && o.nav_modes);
    BOOST_CHECK_EQUAL(*o.sel_alt_ft, 36000);
    BOOST_CHECK_EQUAL(*o.sel_alt_fms_ft, 35008);
    BOOST_CHECK_CLOSE(static_cast<double>(*o.qnh_mb), 1013.2, 0.01);
    BOOST_CHECK_EQUAL(*o.nav_modes, kNavVnav | kNavAltHold);
}

BOOST_AUTO_TEST_CASE(commb_bds50_sets_track_gs_roll) {
    Rig r;
    confirmedWithAlt(r, 35000);
    r.feed(df20(kAddr, ac13Alt(35025), bds50Mb().v), 1.0);
    BOOST_CHECK_EQUAL(r.c().commb_bds50, 1u);
    BOOST_CHECK_EQUAL(r.c().commb_bds40 + r.c().commb_bds60 + r.c().commb_ambiguous + r.c().commb_unknown + r.c().commb_other + r.c().commb_empty, 0u);
    BOOST_REQUIRE(!r.obs.empty());
    const auto& o = r.obs.back();
    BOOST_REQUIRE(o.track_deg && o.gs_kt && o.roll_deg && o.track_rate_dps && o.tas_kt);
    BOOST_CHECK_CLOSE(static_cast<double>(*o.track_deg), 271.4, 0.1);
    BOOST_CHECK_EQUAL(*o.gs_kt, 452.0f);
    BOOST_CHECK_CLOSE(static_cast<double>(*o.roll_deg), -12.3, 0.5);
    BOOST_CHECK_CLOSE(static_cast<double>(*o.track_rate_dps), -0.5, 0.5);
    BOOST_CHECK_EQUAL(*o.tas_kt, 470.0f);
}

BOOST_AUTO_TEST_CASE(commb_unconfirmed_address_counts_ap_unconfirmed) {
    Rig r;
    r.feed(df20(kAddr, ac13Alt(35000), bds60Mb().v), 0.0);
    BOOST_CHECK_EQUAL(r.c().ap_unconfirmed, 1u);
    BOOST_CHECK_EQUAL(r.c().commb_bds60, 0u);
    BOOST_CHECK(r.obs.empty());
}

BOOST_AUTO_TEST_CASE(ambiguous_counts_and_sets_nothing) {
    Rig r;
    r.feed(df17(kAddr, identification(4, 3, "KLM1023")), 0.0);
    r.feed(df4(kAddr, ac13Alt(2000)), 0.1);
    r.obs.clear();
    r.feed(df20(kAddr, ac13Alt(2025), ambiguousMb().v), 1.0);  // no context: both stay plausible
    BOOST_CHECK_EQUAL(r.c().commb_ambiguous, 1u);
    BOOST_CHECK_EQUAL(r.c().commb_bds50 + r.c().commb_bds60 + r.c().commb_bds40, 0u);
    for (const auto& o : r.obs) {
        BOOST_CHECK(!o.heading_mag_deg && !o.ias_kt && !o.mach && !o.roll_deg && !o.tas_kt && !o.track_deg);
    }
    r.feed(df20(kAddr, ac13Alt(2050), Mb{}.v), 2.0);
    BOOST_CHECK_EQUAL(r.c().commb_empty, 1u);
    Mb junk;
    junk.at(1, 8, 0xFF).at(9, 8, 0xFF).at(17, 8, 0xA5).at(25, 8, 0x5A).at(33, 8, 0xFF).at(41, 8, 0xC3).at(49, 8, 0x81);
    r.feed(df20(kAddr, ac13Alt(2075), junk.v), 3.0);
    BOOST_CHECK_EQUAL(r.c().commb_unknown, 1u);
    Mb ident;  // BDS 2,0: first byte 0x20 and eight 6-bit characters
    ident.at(1, 8, 0x20);
    for (unsigned i = 0; i < 8; ++i) ident.at(9 + 6 * i, 6, 1 + i);
    r.feed(df20(kAddr, ac13Alt(2100), ident.v), 4.0);
    BOOST_CHECK_EQUAL(r.c().commb_other, 1u);
}

BOOST_AUTO_TEST_CASE(ra_lifecycle) {
    Rig r;
    r.feed(df17(kAddr, velocity(0, 0)), 0.0);
    r.feed(df4(kAddr, ac13Alt(10000)), 0.5);
    r.obs.clear();
    // t=1: an RA with ARA bit 1 set and bit 3 set (a preventive "monitor vertical speed"; the text is the decoder's own)
    const auto ra = raOf(0x2000 | 0x0800, false, 0x0);
    r.feed(df16(kAddr, ac13Alt(10025), skycued::testing::acasRaMv(ra).v), 1.0);
    BOOST_CHECK_EQUAL(r.c().acas_ra, 1u);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_REQUIRE(r.obs.back().acas_ra);
    BOOST_CHECK(r.obs.back().acas_ra->active);
    BOOST_CHECK_EQUAL(r.obs.back().acas_ra->ara, ra.ara);
    BOOST_CHECK(!r.obs.back().acas_ra->text.empty());
    BOOST_CHECK(r.obs.back().acas_ra->text != "clear of conflict");
    BOOST_CHECK(r.obs.back().acas_sl && r.obs.back().acas_ri);
    // t=11: RAT
    r.obs.clear();
    r.feed(df16(kAddr, ac13Alt(10025), skycued::testing::acasRaMv(raOf(0x2000 | 0x0800, true)).v), 11.0);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_REQUIRE(r.obs.back().acas_ra);
    BOOST_CHECK(!r.obs.back().acas_ra->active);
    BOOST_CHECK_EQUAL(r.obs.back().acas_ra->text, "clear of conflict");
    // t=46 (35 s after RAT): a change that emits no longer carries the field
    r.obs.clear();
    r.feed(df4(kAddr, ac13Alt(10100)), 46.0);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK(!r.obs.back().acas_ra);
}

BOOST_AUTO_TEST_CASE(tc28_st2_ra) {
    Rig r;
    r.feed(df17(kAddr, velocity(0, 0)), 0.0);
    r.feed(df4(kAddr, ac13Alt(10000)), 0.5);
    r.obs.clear();
    const auto ra = raOf(0x2000 | 0x0400, false, 0x5);
    const auto before = r.c().decoded;
    r.feed(df17(kAddr, skycued::testing::acasRaTc28(ra)), 1.0);
    BOOST_CHECK_EQUAL(r.c().acas_ra_tc28, 1u);
    BOOST_CHECK_EQUAL(r.c().acas_ra, 0u);
    BOOST_CHECK_EQUAL(r.c().decoded, before + 1);
    BOOST_CHECK_EQUAL(r.c().unmapped, 0u);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_REQUIRE(r.obs.back().acas_ra);
    BOOST_CHECK(r.obs.back().acas_ra->active);
    BOOST_CHECK_EQUAL(r.obs.back().acas_ra->rac, 5);
}

BOOST_AUTO_TEST_CASE(tc19_st3_heading_ias_no_track) {
    Rig r;
    r.feed(df17(kAddr, velocityAirspeed(3, 90.0f, 250, false, -640)), 0.0);
    r.feed(df4(kAddr, ac13Alt(10000)), 0.5);
    r.feed(df4(kAddr, ac13Alt(10100)), 2.0);
    BOOST_REQUIRE(!r.obs.empty());
    const auto& o = r.obs.back();
    BOOST_REQUIRE(o.heading_mag_deg && o.ias_kt);
    BOOST_CHECK_SMALL(static_cast<double>(*o.heading_mag_deg) - 90.0, 0.5);
    BOOST_CHECK_EQUAL(*o.ias_kt, 250);
    BOOST_CHECK(!o.tas_kt);
    BOOST_CHECK(!o.track_deg);
    BOOST_CHECK(!o.gs_kt);
    // TAS flag and subtype 4 (4 kt steps)
    Rig q;
    q.feed(df17(kAddr, velocityAirspeed(4, 180.0f, 400, true)), 0.0);
    q.feed(df4(kAddr, ac13Alt(10000)), 0.5);
    q.feed(df4(kAddr, ac13Alt(10100)), 2.0);
    BOOST_REQUIRE(!q.obs.empty());
    BOOST_REQUIRE(q.obs.back().tas_kt);
    BOOST_CHECK_EQUAL(*q.obs.back().tas_kt, 400);
    BOOST_CHECK(!q.obs.back().ias_kt);
}


namespace {
skycued::decode::TargetState tsOf(bool valid, bool ap, bool vnav, bool lnav, bool tcas, int32_t sel_alt = 36000) {
    skycued::decode::TargetState t;
    t.sel_alt_ft = sel_alt;
    t.modes_valid = valid;
    t.autopilot = ap;
    t.vnav = vnav;
    t.lnav = lnav;
    t.tcas = tcas;
    return t;
}
// Confirmed key with a known altitude; observations cleared.
void ready(Rig& r) {
    confirmedWithAlt(r, 35000);
    r.obs.clear();
}
}  // namespace

BOOST_AUTO_TEST_CASE(tie_break_uses_tc19_rate) {
    // No track and no ground speed: only the TC19 barometric rate separates 5,0 from 6,0.
    // The reply's 6,0 baro rate is 0 ft/min.
    {
        Rig r;
        r.feed(df17(kAddr, velocityAirspeed(3, 90.0f, 250, false, -1216)), 0.0);
        r.feed(df4(kAddr, ac13Alt(2000)), 0.1);
        r.feed(df20(kAddr, ac13Alt(2025), ambiguousMb().v), 1.0);
        BOOST_CHECK_EQUAL(r.c().commb_bds50, 1u);
        BOOST_CHECK_EQUAL(r.c().commb_bds60 + r.c().commb_ambiguous, 0u);
    }
    {
        Rig r;
        r.feed(df17(kAddr, velocityAirspeed(3, 90.0f, 250, false, 320)), 0.0);
        r.feed(df4(kAddr, ac13Alt(2000)), 0.1);
        r.feed(df20(kAddr, ac13Alt(2025), ambiguousMb().v), 1.0);
        BOOST_CHECK_EQUAL(r.c().commb_bds60, 1u);
        BOOST_CHECK_EQUAL(r.c().commb_bds50 + r.c().commb_ambiguous, 0u);
    }
}

BOOST_AUTO_TEST_CASE(commb_value_replaced_by_later_adsb_and_then_blocked) {
    Rig r;
    ready(r);
    r.feed(df20(kAddr, ac13Alt(35025), bds50Mb().v), 1.0);  // no ADS-B value: Comm-B is stored
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK_CLOSE(static_cast<double>(*r.obs.back().track_deg), 271.4, 0.1);
    BOOST_CHECK_EQUAL(*r.obs.back().gs_kt, 452.0f);
    r.feed(df17(kAddr, velocity(-141, -141)), 2.0);  // ADS-B replaces it at once (track 225, GS 199)
    r.obs.clear();
    r.feed(df20(kAddr, ac13Alt(35050), bds50Mb().v), 3.0);  // and now blocks the next 5,0 reply
    BOOST_CHECK_EQUAL(r.c().commb_bds50, 2u);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK_CLOSE(static_cast<double>(*r.obs.back().track_deg), 225.0, 0.5);
    BOOST_CHECK_CLOSE(static_cast<double>(*r.obs.back().gs_kt), 199.4, 0.5);
    // Other fields of the register are not blocked.
    BOOST_REQUIRE(r.obs.back().roll_deg && r.obs.back().tas_kt);
    BOOST_CHECK_EQUAL(*r.obs.back().tas_kt, 470.0f);
    // After 60 s without ADS-B the Comm-B value is taken again.
    r.feed(df4(kAddr, ac13Alt(35000)), 70.0);
    r.obs.clear();
    r.feed(df17(kAddr, identification(4, 3, "KLM1023")), 71.0);
    r.feed(df20(kAddr, ac13Alt(35025), bds50Mb().v), 73.0);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK_CLOSE(static_cast<double>(*r.obs.back().track_deg), 271.4, 0.1);
}

BOOST_AUTO_TEST_CASE(commb_rate_precedence_for_baro_and_geom_rates) {
    Rig r;
    ready(r);
    r.feed(df20(kAddr, ac13Alt(35025), bds60Mb().v), 1.0);  // baro -1216, inertial -1184 stored
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK_EQUAL(*r.obs.back().baro_rate_fpm, -1216.0f);
    BOOST_CHECK_EQUAL(*r.obs.back().geom_rate_fpm, -1184.0f);
    r.feed(df17(kAddr, velocity(0, 0, 640, true)), 2.0);   // TC19 baro rate
    r.feed(df17(kAddr, velocity(0, 0, 320, false)), 2.1);  // TC19 geometric rate
    r.obs.clear();
    r.feed(df20(kAddr, ac13Alt(35050), bds60Mb().v), 3.0);
    BOOST_CHECK_EQUAL(r.c().commb_bds60, 2u);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK_EQUAL(*r.obs.back().baro_rate_fpm, 640.0f);
    BOOST_CHECK_EQUAL(*r.obs.back().geom_rate_fpm, 320.0f);
    BOOST_REQUIRE(r.obs.back().heading_mag_deg && r.obs.back().ias_kt && r.obs.back().mach);
}

BOOST_AUTO_TEST_CASE(intent_change_emits_without_position) {
    Rig r;
    ready(r);
    r.feed(df17(kAddr, targetState(tsOf(true, true, false, false, false, 36000))), 1.0);
    BOOST_CHECK(r.obs.empty());  // first appearance: not a change
    r.feed(df17(kAddr, targetState(tsOf(true, true, false, false, false, 36992))), 3.0);
    BOOST_REQUIRE_EQUAL(r.obs.size(), 1u);
    BOOST_CHECK_EQUAL(*r.obs[0].sel_alt_ft, 36992);
    // A BDS 4,0 change emits on its own as well.
    confirmedWithAlt(r, 35000, 69.0);  // keeps the key confirmed and the altitude fresh
    r.feed(df20(kAddr, ac13Alt(35000), bds40Mb().v), 70.0);  // TC29 (t=3) is stale: MCP 36000 changes 36992 -> 36000
    BOOST_REQUIRE_EQUAL(r.obs.size(), 2u);
    BOOST_CHECK_EQUAL(*r.obs[1].sel_alt_ft, 36000);
}

BOOST_AUTO_TEST_CASE(air_data_alone_does_not_emit) {
    Rig r;
    ready(r);
    r.feed(df20(kAddr, ac13Alt(35000), bds60Mb().v), 1.0);  // same altitude: nothing changed
    r.feed(df20(kAddr, ac13Alt(35000), bds50Mb().v), 2.0);
    BOOST_CHECK_EQUAL(r.c().commb_bds60 + r.c().commb_bds50, 2u);
    BOOST_CHECK(r.obs.empty());
    r.feed(df17(kAddr, velocityAirspeed(3, 90.0f, 250, false)), 3.0);
    BOOST_CHECK(r.obs.empty());
}

BOOST_AUTO_TEST_CASE(rat_without_prior_ra_is_ignored) {
    Rig r;
    ready(r);
    r.feed(df16(kAddr, ac13Alt(35000), acasRaMv(raOf(0x2000, true)).v), 1.0);
    BOOST_CHECK(r.obs.empty());  // no dirty
    r.feed(df4(kAddr, ac13Alt(35100)), 2.0);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK(!r.obs.back().acas_ra);
    // The same through TC28 subtype 2.
    r.obs.clear();
    r.feed(df17(kAddr, acasRaTc28(raOf(0x2000, true))), 4.0);
    r.feed(df4(kAddr, ac13Alt(35200)), 5.0);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK(!r.obs.back().acas_ra);
}

BOOST_AUTO_TEST_CASE(ra_expires_without_rat) {
    Rig r;
    ready(r);
    r.feed(df16(kAddr, ac13Alt(35000), acasRaMv(raOf(0x2000 | 0x0800)).v), 1.0);
    r.obs.clear();
    r.feed(df4(kAddr, ac13Alt(35100)), 25.0);  // 24 s: still active
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_REQUIRE(r.obs.back().acas_ra);
    BOOST_CHECK(r.obs.back().acas_ra->active);
    r.obs.clear();
    r.feed(df4(kAddr, ac13Alt(35200)), 40.0);  // 39 s: gone
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK(!r.obs.back().acas_ra);
}

BOOST_AUTO_TEST_CASE(df0_sets_acas_sl_and_ri) {
    Rig r;
    ready(r);
    Bytes f = shortAp(0, 0, ac13Alt(35100), kAddr, false);
    const uint8_t sl = 5, ri = 7;
    f[1] = static_cast<uint8_t>(((sl & 7) << 5) | ((ri & 0xF) >> 1));
    f[2] = static_cast<uint8_t>((f[2] & 0x1F) | ((ri & 1) << 7));
    putParity(f, kAddr);
    r.feed(f, 1.0);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_REQUIRE(r.obs.back().acas_sl && r.obs.back().acas_ri);
    BOOST_CHECK_EQUAL(*r.obs.back().acas_sl, 5);
    BOOST_CHECK_EQUAL(*r.obs.back().acas_ri, 7);
}

BOOST_AUTO_TEST_CASE(tc29_subtype0_is_unmapped) {
    Rig r;
    ready(r);
    Me me = typeOnly(29);  // subtype 0 (version 1)
    r.feed(df17(kAddr, me), 1.0);
    BOOST_CHECK_EQUAL(r.c().unmapped, 1u);
}

BOOST_AUTO_TEST_CASE(tc29_sets_selected_altitude_and_modes) {
    Rig r;
    ready(r);
    auto t = tsOf(true, true, true, true, false);
    t.qnh_mb = 1013.6;
    t.sel_heading_deg = 90.0f;
    t.nac_p = 9;
    r.feed(df17(kAddr, targetState(t)), 1.0);
    BOOST_CHECK_EQUAL(r.c().unmapped, 0u);
    r.feed(df4(kAddr, ac13Alt(35100)), 2.0);
    BOOST_REQUIRE(!r.obs.empty());
    const auto& o = r.obs.back();
    BOOST_REQUIRE(o.sel_alt_ft && o.qnh_mb && o.sel_heading_deg && o.nav_modes);
    BOOST_CHECK_EQUAL(*o.sel_alt_ft, 36000);
    BOOST_CHECK(!o.sel_alt_fms_ft);
    BOOST_CHECK_SMALL(static_cast<double>(*o.sel_heading_deg) - 90.0, 1.0);
    BOOST_CHECK_EQUAL(*o.nav_modes, kNavAutopilot | kNavLnav | kNavVnav);
    BOOST_CHECK(!o.nac_p);  // NACp stays TC31-only
    // Flags set but the status bit clear: nothing is taken.
    Rig q;
    ready(q);
    q.feed(df17(kAddr, targetState(tsOf(false, true, true, true, true))), 1.0);
    q.feed(df4(kAddr, ac13Alt(35100)), 2.0);
    BOOST_REQUIRE(!q.obs.empty());
    BOOST_CHECK(!q.obs.back().nav_modes);
    BOOST_CHECK(q.obs.back().sel_alt_ft);
}

BOOST_AUTO_TEST_CASE(nav_modes_replace_rules) {
    // The whole set is replaced by the last message with valid mode bits; no merging.
    // A TC29 under 60 s old outranks BDS 4,0 for the intent fields.
    Rig r;
    ready(r);
    r.feed(df17(kAddr, targetState(tsOf(true, true, false, true, true))), 1.0);
    r.feed(df20(kAddr, ac13Alt(35025), bds40Mb().v), 2.0);  // vnav + althold, ignored: TC29 is fresh
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK_EQUAL(*r.obs.back().nav_modes, kNavAutopilot | kNavLnav | kNavTcas);
    BOOST_CHECK_EQUAL(*r.obs.back().sel_alt_ft, 36000);
    BOOST_CHECK_EQUAL(r.c().commb_bds40, 1u);
    // A TC29 with the status set and a mode off replaces the whole set.
    r.obs.clear();
    r.feed(df17(kAddr, targetState(tsOf(true, true, false, false, false))), 4.0);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK_EQUAL(*r.obs.back().nav_modes, kNavAutopilot);
    // Once the TC29 is 60 s old, a BDS 4,0 replaces the whole set (autopilot is gone, not merged).
    r.obs.clear();
    confirmedWithAlt(r, 35050, 69.0);
    r.obs.clear();
    r.feed(df20(kAddr, ac13Alt(35050), bds40Mb().v), 71.0);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK_EQUAL(*r.obs.back().nav_modes, kNavVnav | kNavAltHold);
    // A 4,0 whose mode status bit is clear leaves nav_modes untouched.
    Mb m = bds40Mb();
    m.at(48, 1, 0).at(49, 1, 0).at(50, 1, 0);
    r.obs.clear();
    r.feed(df20(kAddr, ac13Alt(35050), m.v), 73.0);
    BOOST_CHECK_EQUAL(r.c().commb_bds40, 3u);
    r.feed(df4(kAddr, ac13Alt(35200)), 74.5);  // an altitude change emits an observation
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK_EQUAL(*r.obs.back().nav_modes, kNavVnav | kNavAltHold);
    // A later TC29 replaces a 4,0 value at once.
    r.obs.clear();
    r.feed(df17(kAddr, targetState(tsOf(true, false, false, true, false, 36992))), 75.0);
    r.feed(df4(kAddr, ac13Alt(35100)), 76.0);
    BOOST_REQUIRE(!r.obs.empty());
    BOOST_CHECK_EQUAL(*r.obs.back().nav_modes, kNavLnav);
    BOOST_CHECK_EQUAL(*r.obs.back().sel_alt_ft, 36992);
}

BOOST_AUTO_TEST_CASE(qnh_precedence_tc29_over_bds40) {
    Rig r;
    ready(r);
    auto t = tsOf(true, true, false, false, false);
    t.qnh_mb = 1013.6;
    r.feed(df17(kAddr, targetState(t)), 1.0);
    r.feed(df20(kAddr, ac13Alt(35025), bds40Mb().v), 2.0);  // QNH 1013.2, ignored: the TC29 value is fresh
    BOOST_REQUIRE(!r.obs.empty() && r.obs.back().qnh_mb);
    BOOST_CHECK_CLOSE(static_cast<double>(*r.obs.back().qnh_mb), 1013.6, 0.01);
    r.obs.clear();
    confirmedWithAlt(r, 35050, 69.0);
    r.obs.clear();
    r.feed(df20(kAddr, ac13Alt(35050), bds40Mb().v), 71.0);  // the TC29 is 70 s old: BDS 4,0 takes over
    BOOST_REQUIRE(!r.obs.empty() && r.obs.back().qnh_mb);
    BOOST_CHECK_CLOSE(static_cast<double>(*r.obs.back().qnh_mb), 1013.2, 0.01);
}

BOOST_AUTO_TEST_CASE(sel_alt_fms_precedence_tc29_over_bds40) {
    Rig r;
    ready(r);
    skycued::decode::TargetState t;
    t.sel_alt_fms_ft = 36992;
    t.modes_valid = false;
    r.feed(df17(kAddr, targetState(t)), 1.0);
    r.feed(df20(kAddr, ac13Alt(35025), bds40Mb().v), 2.0);  // FMS 35008, ignored: the TC29 value is fresh
    BOOST_REQUIRE(!r.obs.empty() && r.obs.back().sel_alt_fms_ft);
    BOOST_CHECK_EQUAL(*r.obs.back().sel_alt_fms_ft, 36992);
    r.obs.clear();
    confirmedWithAlt(r, 35050, 69.0);
    r.obs.clear();
    r.feed(df20(kAddr, ac13Alt(35050), bds40Mb().v), 71.0);
    BOOST_REQUIRE(!r.obs.empty() && r.obs.back().sel_alt_fms_ft);
    BOOST_CHECK_EQUAL(*r.obs.back().sel_alt_fms_ft, 35008);
}

BOOST_AUTO_TEST_CASE(no_ra_record_is_inactive) {
    Rig r;
    r.feed(df17(kAddr, velocity(0, 0)), 0.0);
    r.feed(df4(kAddr, ac13Alt(10000)), 0.5);
    r.obs.clear();
    r.feed(df16(kAddr, ac13Alt(10025), skycued::testing::acasRaMv(raOf(0x0000)).v), 1.0);  // bit 1 and MTE clear
    BOOST_REQUIRE(!r.obs.empty() && r.obs.back().acas_ra);
    BOOST_CHECK(!r.obs.back().acas_ra->active);
    BOOST_CHECK_EQUAL(r.obs.back().acas_ra->text, "no RA");
}

BOOST_AUTO_TEST_CASE(rssi_ring_matches_both_sample_mappings)
{
    using skycued::decode::RssiRing;
    RssiRing r;
    BOOST_CHECK(!r.meanDb());
    for (int i = 0; i < 8; ++i) r.pushPower(RssiRing::fromBeastByte(128));
    BOOST_REQUIRE(r.meanDb());
    BOOST_CHECK_EQUAL(*r.meanDb(), -6.0);

    RssiRing u;
    u.pushPower(RssiRing::fromDbfs(-10));
    u.pushPower(RssiRing::fromDbfs(-20));
    u.pushPower(RssiRing::fromDbfs(-30));
    BOOST_REQUIRE(u.meanDb());
    BOOST_CHECK_EQUAL(*u.meanDb(), -14.3);

    RssiRing z;
    z.pushPower(0.0);
    BOOST_CHECK(!z.meanDb());
    RssiRing n;
    n.pushPower(NAN);
    BOOST_CHECK(!n.meanDb());
}

BOOST_AUTO_TEST_CASE(emit_gate_rule_7_2)
{
    using skycued::decode::EmitGate;
    using skycued::decode::TimePoint;
    using skycued::decode::shouldEmitWithoutPosition;
    const TimePoint t0 = std::chrono::time_point_cast<TimePoint::duration>(at(0.0));
    const auto plus = [&](double s) {
        return t0 + std::chrono::duration_cast<TimePoint::duration>(std::chrono::duration<double>(s));
    };
    EmitGate clean;
    BOOST_CHECK(!shouldEmitWithoutPosition(clean, t0));

    EmitGate fresh;
    fresh.dirty = true;
    BOOST_CHECK(shouldEmitWithoutPosition(fresh, t0));
    BOOST_CHECK(fresh.last_change_emit && *fresh.last_change_emit == t0);

    EmitGate g;
    g.dirty = true;
    g.last_accept_at = t0;
    BOOST_CHECK(!shouldEmitWithoutPosition(g, plus(10.0)));
    BOOST_CHECK(shouldEmitWithoutPosition(g, plus(10.001)));
    BOOST_CHECK(!shouldEmitWithoutPosition(g, plus(10.501)));
    BOOST_CHECK(shouldEmitWithoutPosition(g, plus(11.001)));
}

BOOST_AUTO_TEST_CASE(within_range_boundary)
{
    using skycued::decode::withinRange;
    const cpr::Position origin{0.0, 0.0};
    BOOST_CHECK(withinRange({0.0, 10.0}, std::nullopt));
    // Near the equator one degree of longitude is about 60 NM.
    double lo = 0.0, hi = 20.0;
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (lo + hi);
        (cpr::distanceNm({0.0, mid}, origin) <= skycued::decode::limits::kRangeNm ? lo : hi) = mid;
    }
    BOOST_CHECK(withinRange({0.0, lo}, origin));
    BOOST_CHECK(!withinRange({0.0, hi + 1e-6}, origin));
}
