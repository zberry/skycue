// CPR position math. Equations: ICAO Annex 10 Vol IV and The 1090 MHz Riddle
// (position chapter). The encoder below is test-only.
#define BOOST_TEST_MODULE decode_cpr
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "decode/cpr.hpp"
#include "decode/frame.hpp"

using namespace skycued::decode;
using skycued::decode::cpr::Msg;
using skycued::decode::cpr::Position;

namespace {

const double kPi = 3.14159265358979323846;

double posMod(double a, double b) { return a - b * std::floor(a / b); }

// Test-only NL transition latitude: the latitude above which NL drops below n.
double transitionLat(int n) {
    if (n >= 60) return 0.0;
    double c = std::sqrt((1.0 - std::cos(kPi / 30.0)) / (1.0 - std::cos(2.0 * kPi / n)));
    return std::acos(c) * 180.0 / kPi;
}

Msg encodeCpr(const Position& p, bool odd, bool surface) {
    const double zone = surface ? 90.0 : 360.0;
    const double dlat = zone / (odd ? 59.0 : 60.0);
    const double yz = std::floor(131072.0 * (posMod(p.lat, dlat) / dlat) + 0.5);
    const double rlat = dlat * (std::floor(p.lat / dlat) + yz / 131072.0);
    const int nlv = skycued::decode::cpr::nl(rlat);
    const int ni = std::max(nlv - (odd ? 1 : 0), 1);
    const double dlon = zone / ni;
    const double xz = std::floor(131072.0 * (posMod(p.lon, dlon) / dlon) + 0.5);
    return Msg{static_cast<uint32_t>(yz) & 0x1FFFFu, static_cast<uint32_t>(xz) & 0x1FFFFu, odd};
}

Msg fromHex(const std::string& hex) {
    std::vector<uint8_t> b;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        b.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    auto f = classify(b);
    BOOST_REQUIRE(f);
    auto c = decodeCpr(*f);
    BOOST_REQUIRE(c);
    return Msg{c->lat17, c->lon17, c->odd};
}

std::vector<double> readSample() {
    std::ifstream in(std::string(SKYCUE_TEST_DATA_DIR) + "/cpr_nl_transitions.json");
    BOOST_REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    std::vector<double> v;
    const char* p = s.c_str();
    char* end = nullptr;
    for (; *p; ++p) {
        if ((*p >= '0' && *p <= '9') || *p == '-') {
            v.push_back(std::strtod(p, &end));
            p = end - 1;
        }
    }
    return v;
}

}  // namespace

BOOST_AUTO_TEST_CASE(nl_transitions_match_published_table) {
    auto fx = readSample();
    BOOST_REQUIRE_EQUAL(fx.size(), 59u);
    // fx[i] is the transition for n = 60 - i.
    for (int n = 60; n >= 2; --n)
        BOOST_CHECK_SMALL(fx[60 - n] - transitionLat(n), 1e-6);
    // A few values quoted in the Riddle's table.
    BOOST_CHECK_SMALL(fx[1] - 10.47047130, 1e-6);
    BOOST_CHECK_SMALL(fx[2] - 14.82817437, 1e-6);
    BOOST_CHECK_SMALL(fx[57] - 86.53536998, 1e-6);
    BOOST_CHECK_SMALL(fx[58] - 87.0, 1e-6);
    // nl() agrees on both sides of every transition.
    for (int n = 59; n >= 2; --n) {
        double t = fx[60 - n];
        BOOST_CHECK_EQUAL(skycued::decode::cpr::nl(t - 1e-4), n);
        BOOST_CHECK_EQUAL(skycued::decode::cpr::nl(t + 1e-4), n - 1);
        BOOST_CHECK_EQUAL(skycued::decode::cpr::nl(-(t - 1e-4)), n);
    }
    BOOST_CHECK_EQUAL(skycued::decode::cpr::nl(0.0), 59);
}

BOOST_AUTO_TEST_CASE(nl_is_one_at_the_poles) {
    using skycued::decode::cpr::nl;
    BOOST_CHECK_EQUAL(nl(87.5), 1);
    BOOST_CHECK_EQUAL(nl(-87.5), 1);
    BOOST_CHECK_EQUAL(nl(90.0), 1);
    BOOST_CHECK_EQUAL(nl(-90.0), 1);
}

BOOST_AUTO_TEST_CASE(global_airborne_both_orders) {
    Msg even = fromHex("8D40621D58C382D690C8AC2863A7");
    Msg odd = fromHex("8D40621D58C386435CC412692AD6");
    BOOST_REQUIRE(!even.odd);
    BOOST_REQUIRE(odd.odd);
    // The pair was received at different times, so each order gives the
    // position of its newer message: the Riddle quotes 52.25720, 3.91937 for
    // the even-newer order; the odd-newer one lies about 0.6 NM further on.
    auto pe = skycued::decode::cpr::globalAirborne(even, odd, false);
    BOOST_REQUIRE(pe);
    BOOST_CHECK_SMALL(pe->lat - 52.25720, 1e-5);
    BOOST_CHECK_SMALL(pe->lon - 3.91937, 1e-5);
    BOOST_CHECK_SMALL(pe->lat - 52.2572, 1e-4);
    BOOST_CHECK_SMALL(pe->lon - 3.9190, 5e-4);
    auto po = skycued::decode::cpr::globalAirborne(even, odd, true);
    BOOST_REQUIRE(po);
    BOOST_CHECK_LT(skycued::decode::cpr::distanceNm(*po, *pe), 1.0);
    BOOST_CHECK_GT(skycued::decode::cpr::distanceNm(*po, *pe), 0.3);
}

BOOST_AUTO_TEST_CASE(global_airborne_nl_mismatch_rejected) {
    // Either side of the NL 59/58 boundary at 10.47047 degrees.
    Msg even = encodeCpr({10.46, 20.0}, false, false);
    Msg odd = encodeCpr({10.48, 20.0}, true, false);
    BOOST_CHECK(!skycued::decode::cpr::globalAirborne(even, odd, true));
    BOOST_CHECK(!skycued::decode::cpr::globalAirborne(even, odd, false));
}

BOOST_AUTO_TEST_CASE(local_within_half_zone) {
    Msg even = fromHex("8D40621D58C382D690C8AC2863A7");
    Msg odd = fromHex("8D40621D58C386435CC412692AD6");
    auto g = skycued::decode::cpr::globalAirborne(even, odd, true);
    BOOST_REQUIRE(g);
    // Reference 50 NM north (1 degree of latitude is 60 NM).
    Position ref{g->lat + 50.0 / 60.0, g->lon};
    BOOST_CHECK_CLOSE(skycued::decode::cpr::distanceNm(*g, ref), 50.0, 1.0);
    auto l = skycued::decode::cpr::local(odd, ref, false);
    BOOST_REQUIRE(l);
    BOOST_CHECK_SMALL(l->lat - g->lat, 1e-5);
    BOOST_CHECK_SMALL(l->lon - g->lon, 1e-5);
    auto le = skycued::decode::cpr::local(even, ref, false);
    BOOST_REQUIRE(le);
    BOOST_CHECK_LT(skycued::decode::cpr::distanceNm(*le, *g), 1.0);
}

BOOST_AUTO_TEST_CASE(local_beyond_180nm_differs) {
    Msg odd = fromHex("8D40621D58C386435CC412692AD6");
    Msg even = fromHex("8D40621D58C382D690C8AC2863A7");
    auto g = skycued::decode::cpr::globalAirborne(even, odd, true);
    BOOST_REQUIRE(g);
    Position ref{g->lat + 400.0 / 60.0, g->lon};
    auto l = skycued::decode::cpr::local(odd, ref, false);
    BOOST_REQUIRE(l);
    BOOST_CHECK_GT(skycued::decode::cpr::distanceNm(*l, *g), 1.0);
}

BOOST_AUTO_TEST_CASE(global_surface_quadrant_from_reference) {
    const Position truth{52.31, 4.76};
    Msg even = encodeCpr(truth, false, true);
    Msg odd = encodeCpr(truth, true, true);
    const Position refs[4] = {{52.3, 4.7}, {52.3, 94.7}, {-37.7, 4.7}, {-37.7, -85.3}};
    Position out[4];
    for (int i = 0; i < 4; ++i) {
        auto p = skycued::decode::cpr::globalSurface(even, odd, true, refs[i]);
        BOOST_REQUIRE(p);
        out[i] = *p;
    }
    BOOST_CHECK_LT(skycued::decode::cpr::distanceNm(out[0], truth), 1.0);
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            BOOST_CHECK_GT(skycued::decode::cpr::distanceNm(out[i], out[j]), 10.0);
    // The first reference selects the true quadrant for the older-first order too.
    auto p = skycued::decode::cpr::globalSurface(even, odd, false, refs[0]);
    BOOST_REQUIRE(p);
    BOOST_CHECK_LT(skycued::decode::cpr::distanceNm(*p, truth), 1.0);
}

BOOST_AUTO_TEST_CASE(latitude_wrap_and_longitude_normalisation) {
    // Southern latitude: the raw airborne result is >= 270 and wraps by -360.
    Position s{-33.9, 151.2};
    auto g = skycued::decode::cpr::globalAirborne(encodeCpr(s, false, false),
                                                  encodeCpr(s, true, false), true);
    BOOST_REQUIRE(g);
    BOOST_CHECK_SMALL(g->lat - s.lat, 1e-4);
    BOOST_CHECK_LT(g->lat, 0.0);
    // Longitude near the antimeridian stays inside [-180, 180).
    for (double lon : {179.97, -179.97, -180.0, 179.99}) {
        Position w{40.0, lon};
        auto l = skycued::decode::cpr::local(encodeCpr(w, true, false), {40.0, lon}, false);
        BOOST_REQUIRE(l);
        BOOST_CHECK_GE(l->lon, -180.0);
        BOOST_CHECK_LT(l->lon, 180.0);
        double d = std::fabs(posMod(l->lon - lon + 180.0, 360.0) - 180.0);
        BOOST_CHECK_SMALL(d, 1e-4);
    }
    auto g2 = skycued::decode::cpr::globalAirborne(encodeCpr({10.0, -179.99}, false, false),
                                                   encodeCpr({10.0, -179.99}, true, false), false);
    BOOST_REQUIRE(g2);
    BOOST_CHECK_GE(g2->lon, -180.0);
    BOOST_CHECK_LT(g2->lon, 180.0);
}

BOOST_AUTO_TEST_CASE(all_hemispheres) {
    // 17-bit quantisation is 4.6e-5 degrees in latitude, so allow 1e-4.
    const Position cases[] = {{52.3, 4.8},    {-33.9, 151.2}, {40.7, -74.0}, {-34.6, -58.4},
                              {64.1, -21.9},  {-54.8, -68.3}, {1.3, 103.8},  {0.0, 0.0}};
    for (const auto& c : cases) {
        for (bool newer_odd : {false, true}) {
            auto g = skycued::decode::cpr::globalAirborne(encodeCpr(c, false, false),
                                                          encodeCpr(c, true, false), newer_odd);
            BOOST_REQUIRE(g);
            BOOST_CHECK_SMALL(g->lat - c.lat, 1e-4);
            BOOST_CHECK_SMALL(g->lon - c.lon, 1e-3);
            for (bool odd : {false, true}) {
                auto l = skycued::decode::cpr::local(encodeCpr(c, odd, false), {c.lat + 0.3, c.lon - 0.3},
                                                     false);
                BOOST_REQUIRE(l);
                BOOST_CHECK_LT(skycued::decode::cpr::distanceNm(*l, c), 0.5);
            }
            auto s = skycued::decode::cpr::globalSurface(encodeCpr(c, false, true),
                                                         encodeCpr(c, true, true), newer_odd, c);
            BOOST_REQUIRE(s);
            BOOST_CHECK_LT(skycued::decode::cpr::distanceNm(*s, c), 0.5);
            for (bool odd : {false, true}) {
                auto l = skycued::decode::cpr::local(encodeCpr(c, odd, true), {c.lat + 0.1, c.lon + 0.1},
                                                     true);
                BOOST_REQUIRE(l);
                BOOST_CHECK_LT(skycued::decode::cpr::distanceNm(*l, c), 0.5);
            }
        }
    }
}
