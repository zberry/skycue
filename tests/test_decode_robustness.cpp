// The 1090 decoder against damaged input: random bytes, flipped bits, a full
// key table, idle keys and frames of the wrong length (frame decoder design,
// sections 3 and 5). Every PRNG here has a fixed seed.
#define BOOST_TEST_MODULE decode_robustness
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <vector>

#include "decode/modes_decoder.hpp"
#include "decode_test_support.hpp"
#include "geo.hpp"

using namespace skycued::testing;
using skycued::TrackObservation;
using skycued::decode::DecodeCounters;
using skycued::decode::DecoderConfig;
using skycued::decode::ModesDecoder;

namespace {

ModesDecoder::Clock::time_point at(double s) {
    using namespace std::chrono;
    return ModesDecoder::Clock::time_point(duration_cast<ModesDecoder::Clock::duration>(duration<double>(1.7e9 + s)));
}

// A clean synthetic flight: 200 even/odd position pairs, one frame a second,
// moving about 270 kt north-east, with an identification and a velocity
// message ahead of the positions.
std::vector<Bytes> makeFlight() {
    constexpr uint32_t addr = 0x4840D6;
    std::vector<Bytes> fr;
    fr.push_back(df17(addr, identification(4, 3, "KLM1023")));
    fr.push_back(df17(addr, velocity(150, 150)));
    for (int k = 0; k < 200; ++k) {
        const cpr_pos p{52.0 + 0.002 * k, 4.0 + 0.002 * k};
        fr.push_back(df17(addr, airbornePosition(11, encodeCpr(p, false, false), 30000)));
        fr.push_back(df17(addr, airbornePosition(11, encodeCpr(p, true, false), 30000)));
    }
    return fr;
}

struct Run {
    DecodeCounters counters;
    std::map<size_t, std::pair<double, double>> pos;  // frame index -> accepted position
};

Run decodeAll(const std::vector<Bytes>& frames) {
    ModesDecoder dec(DecoderConfig{}, "in1");
    Run r;
    size_t idx = 0;
    dec.onObservation([&](const TrackObservation& o) {
        if (o.lat && o.lon) r.pos[idx] = {*o.lat, *o.lon};
    });
    for (idx = 0; idx < frames.size(); ++idx) dec.feed(frames[idx], at(static_cast<double>(idx)), std::nullopt);
    r.counters = dec.counters();
    return r;
}

}  // namespace

BOOST_AUTO_TEST_CASE(random_bytes_never_throw) {
    std::mt19937 rng(0x5C0DEu);
    ModesDecoder dec(DecoderConfig{}, "in1");
    std::vector<uint8_t> buf(10u * 1024 * 1024);
    for (auto& b : buf) b = static_cast<uint8_t>(rng());
    size_t pos = 0, frames = 0;
    double t = 0;
    BOOST_CHECK_NO_THROW({
        while (pos < buf.size()) {
            const size_t n = (rng() & 1u) ? 14u : 7u;
            if (pos + n > buf.size()) break;
            dec.feed(std::vector<uint8_t>(buf.begin() + static_cast<long>(pos), buf.begin() + static_cast<long>(pos + n)),
                     at(t), std::nullopt);
            t += 0.001;
            pos += n;
            ++frames;
        }
    });
    const auto& c = dec.counters();
    BOOST_CHECK_GT(frames, 900000u);
    BOOST_CHECK_EQUAL(c.frames, frames);
    // Every frame lands in exactly one of the terminal outcomes.
    BOOST_CHECK_EQUAL(c.crc_fail + c.ap_unconfirmed + c.unknown_type + c.no_position + c.cpr_pending +
                          c.cpr_rejected + c.decoded + c.unmapped,
                      c.frames);
}

BOOST_AUTO_TEST_CASE(bit_flips_never_move_a_position_more_than_1nm) {
    const std::vector<Bytes> clean = makeFlight();
    const Run ref = decodeAll(clean);
    BOOST_REQUIRE(ref.pos.size() >= 190u);
    BOOST_CHECK_EQUAL(ref.counters.crc_fixed, 0u);

    std::mt19937 rng(0xB17F11Fu);
    for (int flips = 1; flips <= 3; ++flips) {
        std::vector<Bytes> bad = clean;
        for (auto& f : bad) {
            // Flip distinct bits outside the 5 DF bits.
            std::vector<unsigned> chosen;
            const unsigned nbits = static_cast<unsigned>(f.size()) * 8u;
            while (chosen.size() < static_cast<size_t>(flips)) {
                const unsigned b = 5u + static_cast<unsigned>(rng() % (nbits - 5u));
                if (std::find(chosen.begin(), chosen.end(), b) == chosen.end()) chosen.push_back(b);
            }
            for (unsigned b : chosen) f[b / 8] ^= static_cast<uint8_t>(0x80u >> (b % 8));
        }
        const Run got = decodeAll(bad);
        if (flips == 1) BOOST_CHECK_EQUAL(got.counters.crc_fixed, bad.size());
        size_t checked = 0;
        for (const auto& [idx, p] : got.pos) {
            // The clean decode at the same frame, else the nearest clean one in time.
            const std::pair<double, double>* want = nullptr;
            size_t best = std::numeric_limits<size_t>::max();
            for (const auto& [ci, cp] : ref.pos) {
                const size_t d = ci > idx ? ci - idx : idx - ci;
                if (d < best) {
                    best = d;
                    want = &cp;
                }
            }
            BOOST_REQUIRE(want != nullptr);
            const double nm = skycued::geo::greatCircleNm(p.first, p.second, want->first, want->second);
            BOOST_CHECK_MESSAGE(nm <= 1.0, "flips=" << flips << " frame " << idx << " moved " << nm << " NM");
            ++checked;
        }
        BOOST_TEST_MESSAGE("flips=" << flips << ": " << checked << " positions checked, crc_fail "
                                    << got.counters.crc_fail << ", crc_fixed " << got.counters.crc_fixed);
    }
}

BOOST_AUTO_TEST_CASE(key_table_full_evicts_oldest) {
    DecoderConfig cfg;
    cfg.max_keys = 100;
    ModesDecoder dec(cfg, "in1");
    for (uint32_t i = 0; i < 101; ++i) dec.feed(df17(0x100000 + i, identification(4, 3, "TEST")), at(i * 0.1), std::nullopt);
    BOOST_CHECK_EQUAL(dec.counters().evicted, 1u);
    BOOST_CHECK_EQUAL(dec.keys(), 100u);
    // The first address is gone: a new frame from it creates a key again and evicts the next oldest.
    dec.feed(df17(0x100000, identification(4, 3, "TEST")), at(11.0), std::nullopt);
    BOOST_CHECK_EQUAL(dec.counters().evicted, 2u);
    BOOST_CHECK_EQUAL(dec.keys(), 100u);
}

BOOST_AUTO_TEST_CASE(idle_keys_evicted_after_300s) {
    ModesDecoder dec(DecoderConfig{}, "in1");
    dec.feed(df17(0x100001, identification(4, 3, "TEST")), at(0.0), std::nullopt);
    dec.feed(df17(0x100002, identification(4, 3, "TEST")), at(100.0), std::nullopt);
    BOOST_CHECK_EQUAL(dec.keys(), 2u);
    dec.feed(df17(0x100002, identification(4, 3, "TEST")), at(300.0), std::nullopt);
    BOOST_CHECK_EQUAL(dec.keys(), 2u);  // exactly 300 s idle is still kept
    dec.feed(df17(0x100002, identification(4, 3, "TEST")), at(301.0), std::nullopt);
    BOOST_CHECK_EQUAL(dec.keys(), 1u);
    BOOST_CHECK_EQUAL(dec.counters().evicted, 1u);
}

BOOST_AUTO_TEST_CASE(truncated_and_oversize_frames_are_unknown_type) {
    ModesDecoder dec(DecoderConfig{}, "in1");
    for (size_t n : {6u, 8u, 13u, 15u}) dec.feed(Bytes(n, 0x8D), at(0.0), std::nullopt);
    BOOST_CHECK_EQUAL(dec.counters().frames, 4u);
    BOOST_CHECK_EQUAL(dec.counters().unknown_type, 4u);
    BOOST_CHECK_EQUAL(dec.keys(), 0u);
}
