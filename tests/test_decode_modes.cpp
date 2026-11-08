// The 1090 decoder: address confirmation, CPR pairing and observation
// production (frame decoder design, sections 5 and 7).
#define BOOST_TEST_MODULE decode_modes
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cmath>
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

struct Rig {
    explicit Rig(DecoderConfig cfg = {}) : dec(cfg, "in1") {
        dec.onObservation([this](const TrackObservation& o) { obs.push_back(o); });
    }
    void feed(const Bytes& b, double t, std::optional<uint8_t> sig = std::nullopt) { dec.feed(b, at(t), sig); }
    const skycued::decode::DecodeCounters& c() const { return dec.counters(); }
    ModesDecoder dec;
    std::vector<TrackObservation> obs;
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
