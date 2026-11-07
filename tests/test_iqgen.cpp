#define BOOST_TEST_MODULE iqgen
#include <boost/test/unit_test.hpp>

#include "restricted_fixtures.hpp"

#include "gz_line_reader.hpp"

#include "coding/bits.hpp"
#include "inputs.hpp"
#include "iq_writer.hpp"
#include "coding/modes_crc.hpp"
#include "placement.hpp"
#include "reed_solomon.hpp"
#include "resample.hpp"
#include "synth.hpp"
#include "uat_frame.hpp"

#include <stdlib.h>
#include <unistd.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace iqgen;
using namespace skycued::coding;
namespace tt = boost::test_tools;

namespace {

const std::string kFixtures = SKYCUE_FIXTURE_DIR;

std::vector<uint8_t> hex(const std::string& s)
{
    std::vector<uint8_t> out;
    BOOST_REQUIRE_MESSAGE(parseHex(s, out), "bad hex in test: " + s);
    return out;
}

// Bit i (0 = first transmitted) of a 0/1-per-element vector.
unsigned bitOf(const std::vector<uint8_t>& bits, std::size_t i)
{
    return bits.at(i);
}

// bits [from, from + n) read MSB first.
uint64_t readBits(const std::vector<uint8_t>& bits, std::size_t from, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v = (v << 1) | bitOf(bits, from + i);
    return v;
}

bool allZero(const std::vector<uint8_t>& v)
{
    for (uint8_t x : v)
        if (x != 0)
            return false;
    return true;
}

// The payloads of raw/uat-sample.txt.gz: the hex between the leading
// '+'/'-' and the first ';'.
std::vector<std::vector<uint8_t>> fixturePayloads()
{
    std::vector<std::vector<uint8_t>> out;
    skycued::GzLineReader in(kFixtures + "/raw/uat-sample.txt.gz");
    for (std::string line; in.getline(line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        BOOST_REQUIRE(line[0] == '+' || line[0] == '-');
        const auto semi = line.find(';');
        out.push_back(hex(line.substr(1, semi == std::string::npos ? std::string::npos : semi - 1)));
    }
    return out;
}

const std::vector<uint8_t>& firstOfKind(const std::vector<std::vector<uint8_t>>& all, UatKind kind)
{
    for (const auto& p : all)
        if (uatKindForPayload(p.size()) == kind)
            return p;
    BOOST_FAIL("fixture has no payload of the wanted kind");
    return all.front();  // not reached
}

std::vector<uint8_t> uplinkBlock(const std::vector<uint8_t>& p, std::size_t j)
{
    std::vector<uint8_t> block(p.begin() + static_cast<std::ptrdiff_t>(72 * j),
                               p.begin() + static_cast<std::ptrdiff_t>(72 * j + 72));
    const auto par = uatCode(UatKind::uplink).parity(&p[72 * j], 72);
    block.insert(block.end(), par.begin(), par.end());
    return block;
}

}  // namespace

BOOST_AUTO_TEST_CASE(hex_round_trip)
{
    std::vector<uint8_t> out{1, 2, 3};
    BOOST_TEST(parseHex("00aBff", out));
    BOOST_TEST(out == (std::vector<uint8_t>{0x00, 0xab, 0xff}));
    BOOST_TEST(toHex(out, false) == "00abff");
    BOOST_TEST(toHex(out, true) == "00ABFF");
    BOOST_TEST(parseHex("", out));
    BOOST_TEST(out.empty());

    out = {7};
    BOOST_TEST(!parseHex("abc", out));   // odd length
    BOOST_TEST(!parseHex("0g", out));    // not hex
    BOOST_TEST(!parseHex("0x00", out));
    BOOST_TEST(out == std::vector<uint8_t>{7});  // untouched on failure
}

BOOST_AUTO_TEST_CASE(modes_crc_vectors)
{
    BOOST_TEST(modesCrcResidual(hex("8D4840D6202CC371C32CE0576098")) == 0u);
    BOOST_TEST(modesCrcResidual(hex("5D486258DD7987")) == 0u);         // DF11, IID 0 (fixture)
    BOOST_TEST(modesCrcResidual(hex("5F406A936F50F0")) == 0x000007u);  // DF11, IID 7 (fixture line 1)
    BOOST_TEST(modesCrcResidual(hex("A00007318DB436F0A80000A7959E")) == 0x4B8E45u);  // DF20 AP
    auto f = hex("8D4840D6202CC371C32CE0576098");
    f[3] ^= 0x10;
    BOOST_TEST(modesCrcResidual(f) != 0u);
}

BOOST_AUTO_TEST_CASE(modes_clean_rule)
{
    BOOST_TEST(modesClean(hex("8D4840D6202CC371C32CE0576098")));
    BOOST_TEST(modesClean(hex("5D486258DD7987")));
    BOOST_TEST(!modesClean(hex("5F406A936F50F0")));                // IID != 0
    BOOST_TEST(!modesClean(hex("A00007318DB436F0A80000A7959E")));  // DF20
    BOOST_TEST(modesDf(hex("A00007318DB436F0A80000A7959E")) == 20);
    BOOST_TEST(modesDf(hex("8D4840D6202CC371C32CE0576098")) == 17);
    BOOST_TEST(modesDf(hex("5D486258DD7987")) == 11);
    BOOST_TEST(modesDf({}) == -1);
}

BOOST_AUTO_TEST_CASE(rs_codes_match_dump978)
{
    BOOST_TEST(uatCode(UatKind::downlink_short).nroots() == 12u);
    BOOST_TEST(uatCode(UatKind::downlink_long).nroots() == 14u);
    BOOST_TEST(uatCode(UatKind::uplink).nroots() == 20u);
}

// Known answers: the first payload of each kind in raw/uat-sample.txt.gz and
// the parity dump978's own Reed-Solomon library (configured as in dump978's
// fec.cc) computes for it, recorded from a one-off cross-check outside the
// tree. Unlike the syndrome tests, these pin the field polynomial, fcr and
// prim: a wrong one gives self-consistent codewords with different parity.
BOOST_AUTO_TEST_CASE(rs_known_answer_parity)
{
    struct Vector {
        UatKind kind;
        const char* data;
        const char* parity;
    };
    const Vector vectors[] = {
        {UatKind::downlink_short, "00a66ef135445d525a0c0519119021204800", "6cb82bc4d53a5b2bb0a8ec6e"},
        {UatKind::downlink_long, "08a66ef1353e2d525fd4050911882aa038101d06b85d440be2a4c2a0000590000000",
         "d0e3c7ccb1fed50a5afd9d6aa963"},
        {UatKind::uplink,  // block 0 of the first uplink
         "3514c952d65ca7b0158000210de09082102d30cb00082f0d1e012d30cb000000000000000fd900011710120118173ba9c9635e4c"
         "00158000210e9e0082102cf04b00082f521e012c",
         "0b1400618e79e05a1f9f713cc76928071ea4a6eb"},
    };
    for (const auto& v : vectors) {
        const auto data = hex(v.data);
        const auto expected = hex(v.parity);
        const auto got = uatCode(v.kind).parity(data.data(), data.size());
        BOOST_TEST(got == expected, tt::per_element());
        BOOST_TEST(toHex(got, false) == v.parity);
    }
}

// Every line of raw/uat-sample.txt.gz:
// downlink: codeword = payload + parity, all syndromes 0, first k bytes == payload;
// uplink: each block j (bytes 72j..72j+71 + its 20 parity bytes) has all 20 syndromes 0.
BOOST_AUTO_TEST_CASE(rs_every_fixture_codeword_has_zero_syndromes, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto all = fixturePayloads();
    std::size_t n_short = 0, n_long = 0, n_up = 0;
    for (const auto& p : all) {
        const auto kind = uatKindForPayload(p.size());
        BOOST_REQUIRE(kind.has_value());
        const ReedSolomon& rs = uatCode(*kind);
        if (*kind == UatKind::uplink) {
            ++n_up;
            for (std::size_t j = 0; j < 6; ++j) {
                const auto block = uplinkBlock(p, j);
                BOOST_REQUIRE(block.size() == 92u);
                const auto syn = rs.syndromes(block.data(), block.size());
                BOOST_TEST(syn.size() == 20u);
                BOOST_TEST(allZero(syn));
            }
            continue;
        }
        (*kind == UatKind::downlink_short ? n_short : n_long)++;
        const auto par = rs.parity(p.data(), p.size());
        BOOST_TEST(par.size() == rs.nroots());
        std::vector<uint8_t> cw = p;
        cw.insert(cw.end(), par.begin(), par.end());
        const auto syn = rs.syndromes(cw.data(), cw.size());
        BOOST_TEST(syn.size() == rs.nroots());
        BOOST_TEST(allZero(syn));

        const auto enc = uatEncode(p);
        BOOST_TEST(enc == cw);
        BOOST_TEST(std::vector<uint8_t>(enc.begin(), enc.begin() + static_cast<std::ptrdiff_t>(p.size())) == p);
    }
    BOOST_TEST(n_short > 0u);
    BOOST_TEST(n_long > 0u);
    BOOST_TEST(n_up > 0u);
    BOOST_TEST(n_short + n_long + n_up == all.size());
}

// One codeword of each kind: every single-byte corruption at every position
// leaves a non-zero syndrome, and the parity is no longer the data's parity.
BOOST_AUTO_TEST_CASE(rs_corrupted_codeword_has_nonzero_syndrome, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto all = fixturePayloads();
    for (UatKind kind : {UatKind::downlink_short, UatKind::downlink_long, UatKind::uplink}) {
        const auto& p = firstOfKind(all, kind);
        const ReedSolomon& rs = uatCode(kind);
        std::vector<uint8_t> cw;
        if (kind == UatKind::uplink) {
            cw = uplinkBlock(p, 0);
        } else {
            cw = p;
            const auto par = rs.parity(p.data(), p.size());
            cw.insert(cw.end(), par.begin(), par.end());
        }
        BOOST_REQUIRE(allZero(rs.syndromes(cw.data(), cw.size())));
        for (std::size_t i = 0; i < cw.size(); ++i) {
            for (uint8_t e : {uint8_t{0x01}, uint8_t{0x80}, uint8_t{0xff}}) {
                auto bad = cw;
                bad[i] = static_cast<uint8_t>(bad[i] ^ e);
                const auto syn = rs.syndromes(bad.data(), bad.size());
                BOOST_TEST(!allZero(syn), "kind " << static_cast<int>(kind) << " byte " << i);
            }
        }
    }
}

// Properties that hold for any data: zero data has zero parity, parity is
// linear, leading zero bytes (a shortened code) change nothing.
BOOST_AUTO_TEST_CASE(rs_linear_and_shortenable)
{
    const ReedSolomon& rs = uatCode(UatKind::downlink_long);
    const std::vector<uint8_t> zeros(34, 0);
    BOOST_TEST(allZero(rs.parity(zeros.data(), zeros.size())));

    std::vector<uint8_t> a(34), b(34), ab(34);
    for (std::size_t i = 0; i < 34; ++i) {
        a[i] = static_cast<uint8_t>(i * 37 + 11);
        b[i] = static_cast<uint8_t>(i * i + 3);
        ab[i] = static_cast<uint8_t>(a[i] ^ b[i]);
    }
    const auto pa = rs.parity(a.data(), a.size());
    const auto pb = rs.parity(b.data(), b.size());
    const auto pab = rs.parity(ab.data(), ab.size());
    for (std::size_t i = 0; i < pab.size(); ++i)
        BOOST_TEST(pab[i] == (pa[i] ^ pb[i]));

    std::vector<uint8_t> padded(100, 0);
    padded.insert(padded.end(), a.begin(), a.end());
    BOOST_TEST(rs.parity(padded.data(), padded.size()) == pa);

    // A full-length (255-byte) codeword works; a longer one is rejected.
    std::vector<uint8_t> full(255 - 14, 0x5a);
    const auto pf = rs.parity(full.data(), full.size());
    full.insert(full.end(), pf.begin(), pf.end());
    BOOST_TEST(allZero(rs.syndromes(full.data(), full.size())));
    full.push_back(0);
    BOOST_CHECK_THROW(rs.syndromes(full.data(), full.size()), std::invalid_argument);
    BOOST_CHECK_THROW(ReedSolomon(0x187, 120, 1, 0), std::invalid_argument);
    BOOST_CHECK_THROW(ReedSolomon(0x100, 120, 1, 12), std::invalid_argument);  // x^8: not primitive
}

BOOST_AUTO_TEST_CASE(uat_lengths)
{
    BOOST_TEST(!uatKindForPayload(20).has_value());
    BOOST_TEST((uatKindForPayload(18) == UatKind::downlink_short));
    BOOST_TEST((uatKindForPayload(34) == UatKind::downlink_long));
    BOOST_TEST((uatKindForPayload(432) == UatKind::uplink));
    BOOST_TEST(uatEncode(std::vector<uint8_t>(18)).size() == 30u);
    BOOST_TEST(uatEncode(std::vector<uint8_t>(34)).size() == 48u);
    BOOST_TEST(uatEncode(std::vector<uint8_t>(432)).size() == 552u);
    BOOST_TEST(uatFrameBits(std::vector<uint8_t>(18)).size() == 36u + 240u);
    BOOST_TEST(uatFrameBits(std::vector<uint8_t>(34)).size() == 36u + 384u);
    BOOST_TEST(uatFrameBits(std::vector<uint8_t>(432)).size() == 36u + 4416u);
    BOOST_TEST(uatFrameSamples(UatKind::downlink_short) == 552u);
    BOOST_TEST(uatFrameSamples(UatKind::downlink_long) == 840u);
    BOOST_TEST(uatFrameSamples(UatKind::uplink) == 8904u);
    BOOST_CHECK_THROW(uatEncode(std::vector<uint8_t>(20)), std::invalid_argument);
    BOOST_CHECK_THROW(uatFrameBits(std::vector<uint8_t>(20)), std::invalid_argument);
}

// The first 36 bits are the sync word MSB first, then each transmitted byte MSB first.
BOOST_AUTO_TEST_CASE(uat_sync_word_first, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto all = fixturePayloads();
    for (UatKind kind : {UatKind::downlink_short, UatKind::downlink_long, UatKind::uplink}) {
        const auto& p = firstOfKind(all, kind);
        const auto bits = uatFrameBits(p);
        const auto enc = uatEncode(p);
        BOOST_REQUIRE(bits.size() == 36u + 8u * enc.size());
        BOOST_TEST(readBits(bits, 0, 36) == (kind == UatKind::uplink ? 0x153225B1DULL : 0xEACDDA4E2ULL));
        for (std::size_t i = 0; i < enc.size(); ++i)
            BOOST_TEST(readBits(bits, 36 + 8 * i, 8) == enc[i]);
        for (uint8_t b : bits)
            BOOST_TEST(b <= 1u);
        BOOST_TEST(uatFrameSamples(kind) == 2 * bits.size());
    }
}

BOOST_AUTO_TEST_CASE(uplink_interleave, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto all = fixturePayloads();
    const auto& p = firstOfKind(all, UatKind::uplink);
    const auto enc = uatEncode(p);
    BOOST_REQUIRE(enc.size() == 552u);
    for (std::size_t j = 0; j < 6; ++j) {
        const auto block = uplinkBlock(p, j);
        for (std::size_t i = 0; i < 92; ++i)
            BOOST_TEST(enc[6 * i + j] == block[i], "block " << j << " byte " << i);
    }
}

BOOST_AUTO_TEST_CASE(bits_msb_first)
{
    std::vector<uint8_t> bits;
    const uint8_t bytes[] = {0x80, 0x01};
    appendBytes(bits, bytes, 2);
    BOOST_TEST(bits == (std::vector<uint8_t>{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}));

    bits.clear();
    appendBits(bits, 0x5, 3);
    BOOST_TEST(bits == (std::vector<uint8_t>{1, 0, 1}));
    appendBits(bits, 0x2, 2);  // appends
    BOOST_TEST(bits == (std::vector<uint8_t>{1, 0, 1, 1, 0}));

    bits.clear();
    appendBits(bits, 0x8000000000000001ULL, 64);
    BOOST_TEST(bits.size() == 64u);
    BOOST_TEST(bits.front() == 1u);
    BOOST_TEST(bits.back() == 1u);
    BOOST_TEST(readBits(bits, 1, 62) == 0u);
    appendBits(bits, 0xff, 0);
    BOOST_TEST(bits.size() == 64u);
    BOOST_CHECK_THROW(appendBits(bits, 0, 65), std::invalid_argument);
}

// ---- Sample layer: synthesisers, CU8 writer, resampler, info ----

namespace {

const std::string kF112 = "8D4840D6202CC371C32CE0576098";

std::vector<uint8_t> frameBits(const std::string& h)
{
    const std::vector<uint8_t> bytes = hex(h);
    std::vector<uint8_t> bits;
    appendBytes(bits, bytes.data(), bytes.size());
    return bits;
}

// Mean of out[m].real() over the m whose centre 5m + 2.5 (in ticks) lies in
// [start, start + 6), i.e. the half-us chip starting at tick start.
double chipMean(const std::vector<std::complex<float>>& out, int64_t start)
{
    double sum = 0.0;
    int count = 0;
    for (std::size_t m = 0; m < out.size(); ++m) {
        const double centre = 5.0 * static_cast<double>(m) + 2.5;
        if (centre >= static_cast<double>(start) && centre < static_cast<double>(start + 6)) {
            sum += static_cast<double>(out[m].real());
            ++count;
        }
    }
    BOOST_REQUIRE(count > 0);
    return sum / count;
}

// CU8 bytes back to (I - 128, Q - 128).
std::vector<std::complex<double>> cu8Samples(const std::string& bytes)
{
    std::vector<std::complex<double>> out;
    for (std::size_t k = 0; k + 1 < bytes.size(); k += 2)
        out.emplace_back(static_cast<uint8_t>(bytes[k]) - 128.0, static_cast<uint8_t>(bytes[k + 1]) - 128.0);
    return out;
}

std::string writeConstant(std::complex<float> v, std::size_t n, double amplitude, std::optional<double> snr,
                          double cfo, uint64_t seed, double rate = kModesRate)
{
    std::ostringstream os;
    IqWriter w(os, rate, amplitude, snr, cfo, seed);
    const std::vector<std::complex<float>> x(n, v);
    w.write(x.data(), x.size());
    BOOST_TEST(w.samples() == n);
    return os.str();
}

// Mean of arg(x[n+1] conj(x[n])) over n in [from, to - 1).
double meanPhaseStep(const std::vector<std::complex<double>>& x, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (std::size_t n = from; n + 1 < to; ++n)
        sum += std::arg(x[n + 1] * std::conj(x[n]));
    return sum / static_cast<double>(to - 1 - from);
}

uint64_t fnv1a(const std::string& s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : s) {
        h ^= static_cast<uint8_t>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

constexpr double kTestPi = 3.14159265358979323846;

}  // namespace

BOOST_AUTO_TEST_CASE(modes_ppm_after_decimation)
{
    const std::vector<uint8_t> bits = frameBits(kF112);
    BOOST_REQUIRE(bits.size() == 112u);
    for (int64_t off : {0, 1, 2, 3, 4}) {  // fractional timing against the output clock
        BOOST_TEST_CONTEXT("offset " << off)
        {
            ModesSynth s(100.0);
            std::vector<std::complex<float>> out;
            s.addFrame(off, hex(kF112));
            s.emitUntil(off + 1440 + 10, out);
            BOOST_TEST(out.size() == static_cast<std::size_t>((off + 1450) / 5));
            for (int c : {0, 2, 7, 9})  // preamble pulses
                BOOST_TEST(chipMean(out, off + 6 * c) >= 50.0);
            for (int c : {1, 3, 4, 5, 6, 8, 10, 11, 12, 13, 14, 15})
                BOOST_TEST(chipMean(out, off + 6 * c) <= 50.0);
            for (int b = 0; b < 112; ++b) {  // data from 8 us = tick 96
                const double first = chipMean(out, off + 96 + 12 * b);
                const double second = chipMean(out, off + 102 + 12 * b);
                BOOST_TEST((first > second) == (bitOf(bits, static_cast<std::size_t>(b)) == 1u), "bit " << b);
            }
            for (const auto& v : out)
                BOOST_TEST(v.imag() == 0.0f);
            // Nothing after the frame (tick off + 1440 on).
            for (std::size_t m = static_cast<std::size_t>(off + 1440 + 4) / 5; m < out.size(); ++m)
                BOOST_TEST(out[m].real() == 0.0f);
        }
    }
}

BOOST_AUTO_TEST_CASE(modes_frame_ticks)
{
    BOOST_TEST(modesFrameTicks(7) == 768u);
    BOOST_TEST(modesFrameTicks(14) == 1440u);
}

BOOST_AUTO_TEST_CASE(modes_overlapping_frames_sum)
{
    ModesSynth once(100.0), twice(100.0);
    std::vector<std::complex<float>> a, b;
    once.addFrame(0, hex(kF112));
    twice.addFrame(0, hex(kF112));
    twice.addFrame(0, hex(kF112));
    once.emitUntil(1450, a);
    twice.emitUntil(1450, b);
    BOOST_REQUIRE(a.size() == b.size());
    for (int c : {0, 2, 7, 9})
        BOOST_TEST(chipMean(b, 6 * c) >= 2 * 50.0);
    for (std::size_t m = 0; m < a.size(); ++m)
        BOOST_TEST(b[m].real() == 2.0f * a[m].real(), tt::tolerance(1e-5f));
}

BOOST_AUTO_TEST_CASE(synth_ring_bounds)
{
    ModesSynth m(10.0);
    std::vector<std::complex<float>> out;
    m.emitUntil(100, out);
    BOOST_TEST(out.size() == 20u);
    BOOST_TEST(m.emittedTicks() == 100);
    BOOST_CHECK_THROW(m.addFrame(99, hex(kF112)), std::invalid_argument);  // before the emitted ticks
    BOOST_CHECK_THROW(m.addFrame(100 + static_cast<int64_t>(kModesRingTicks) - 1439, hex(kF112)),
                      std::invalid_argument);  // past the ring
    BOOST_CHECK_NO_THROW(m.addFrame(100 + static_cast<int64_t>(kModesRingTicks) - 1440, hex(kF112)));
    // A frame overlapping one already added is fine once emitted up to its start.
    m.emitUntil(1000, out);
    BOOST_CHECK_NO_THROW(m.addFrame(1000, hex(kF112)));

    UatSynth u(10.0);
    u.emitUntil(10, out);
    BOOST_TEST(u.emittedSamples() == 10);
    const std::vector<uint8_t> bits(kUatRingSamples / 2, 1);
    BOOST_CHECK_THROW(u.addFrame(9, bits), std::invalid_argument);
    BOOST_CHECK_THROW(u.addFrame(11, bits), std::invalid_argument);
    BOOST_CHECK_NO_THROW(u.addFrame(10, bits));
}

BOOST_AUTO_TEST_CASE(uat_cpfsk_phase_steps, *skycue_test::needs_fixture("raw/uat-sample.txt.gz"))
{
    const auto all = fixturePayloads();
    const std::vector<uint8_t>* payload = nullptr;
    for (const auto& p : all)
        if (uatKindForPayload(p.size()) && *uatKindForPayload(p.size()) != UatKind::uplink) {
            payload = &p;
            break;
        }
    BOOST_REQUIRE(payload != nullptr);
    const std::vector<uint8_t> bits = uatFrameBits(*payload);
    const std::size_t n_samples = 2 * bits.size();
    BOOST_TEST(n_samples == uatFrameSamples(*uatKindForPayload(payload->size())));

    UatSynth s(50.0);
    std::vector<std::complex<float>> out;
    s.addFrame(0, bits);
    s.emitUntil(static_cast<int64_t>(n_samples) + 20, out);
    BOOST_REQUIRE(out.size() == n_samples + 20);
    BOOST_TEST(std::arg(out[0]) == 0.0f);  // phase starts at 0
    for (std::size_t n = 0; n < n_samples; ++n)
        BOOST_TEST(std::fabs(std::abs(out[n]) - 50.0f) <= 1e-3f, "sample " << n);
    const double step = 0.3 * kTestPi;
    for (std::size_t n = 0; n + 1 < n_samples; ++n) {
        const std::complex<double> a(out[n]), b(out[n + 1]);
        const double expect = bits[n / 2] ? step : -step;
        BOOST_TEST(std::fabs(std::arg(b * std::conj(a)) - expect) <= 1e-4, "sample " << n);
    }
    for (std::size_t n = n_samples; n < out.size(); ++n)
        BOOST_TEST(std::abs(out[n]) == 0.0f);

    // Overlapping frames add as complex values.
    UatSynth s2(50.0);
    std::vector<std::complex<float>> out2;
    s2.addFrame(0, bits);
    s2.addFrame(0, bits);
    s2.emitUntil(static_cast<int64_t>(n_samples), out2);
    for (std::size_t n = 0; n < n_samples; n += 97)
        BOOST_TEST(std::abs(out2[n] - 2.0f * out[n]) <= 1e-4f);
}

BOOST_AUTO_TEST_CASE(writer_level_and_quantise)
{
    BOOST_TEST(amplitudeForLevel(-12.0) == 32.15, tt::tolerance(0.01));
    BOOST_TEST(amplitudeForLevel(0.0) == 128.0, tt::tolerance(1e-12));

    const std::string bytes = writeConstant({32.15f, 0.0f}, 1000, 32.15, std::nullopt, 0.0, 1);
    BOOST_REQUIRE(bytes.size() == 2000u);
    for (std::size_t k = 0; k < bytes.size(); k += 2) {
        BOOST_TEST(static_cast<uint8_t>(bytes[k]) == 160u);
        BOOST_TEST(static_cast<uint8_t>(bytes[k + 1]) == 128u);
    }

    uint64_t clipped = 0;
    BOOST_TEST(quantiseCu8(0.4, clipped) == 128u);
    BOOST_TEST(quantiseCu8(0.5, clipped) == 129u);
    BOOST_TEST(quantiseCu8(-0.5, clipped) == 128u);  // round(127.5), half away from zero
    BOOST_TEST(quantiseCu8(-0.6, clipped) == 127u);
    BOOST_TEST(quantiseCu8(127.4, clipped) == 255u);
    BOOST_TEST(quantiseCu8(-128.0, clipped) == 0u);
    BOOST_TEST(clipped == 0u);
}

BOOST_AUTO_TEST_CASE(writer_noise_sigma)
{
    const double a = 32.15;
    const std::string bytes = writeConstant({0.0f, 0.0f}, 1000000, a, 25.0, 0.0, 1);
    double si = 0, sq = 0, si2 = 0, sq2 = 0;
    for (const auto& x : cu8Samples(bytes)) {
        si += x.real();
        sq += x.imag();
        si2 += x.real() * x.real();
        sq2 += x.imag() * x.imag();
    }
    const double n = 1e6;
    const double sd_i = std::sqrt(si2 / n - (si / n) * (si / n));
    const double sd_q = std::sqrt(sq2 / n - (sq / n) * (sq / n));
    const double s = a * std::pow(10.0, -25.0 / 20.0) / std::sqrt(2.0);
    const double expect = std::sqrt(s * s + 1.0 / 12.0);
    BOOST_TEST(sd_i == expect, tt::tolerance(0.05));
    BOOST_TEST(sd_q == expect, tt::tolerance(0.05));
    BOOST_TEST(std::fabs(si / n) <= 0.01);
    BOOST_TEST(std::fabs(sq / n) <= 0.01);
}

BOOST_AUTO_TEST_CASE(writer_seed_determinism)
{
    const std::string a = writeConstant({20.0f, -10.0f}, 100000, 32.15, 10.0, 0.0, 1);
    const std::string b = writeConstant({20.0f, -10.0f}, 100000, 32.15, 10.0, 0.0, 1);
    const std::string c = writeConstant({20.0f, -10.0f}, 100000, 32.15, 10.0, 0.0, 2);
    BOOST_TEST(a.size() == 200000u);
    BOOST_TEST((a == b));
    BOOST_TEST(c.size() == a.size());
    BOOST_TEST((a != c));
    // Known answer: the same bytes from every compiler and standard library
    // (mt19937_64 is fully specified; Box-Muller is ours).
    // A failure seen only on another libm or architecture: suspect the
    // last-bit rounding of log/sqrt/cos/sin before the generator.
    BOOST_TEST(fnv1a(a) == 0x38638F09A663D608ULL);
}

BOOST_AUTO_TEST_CASE(writer_cfo_rotation)
{
    for (double cfo : {10000.0, -10000.0}) {
        const auto x = cu8Samples(writeConstant({60.0f, 0.0f}, 100000, 60.0, std::nullopt, cfo, 1));
        BOOST_TEST(std::abs(x[0] - std::complex<double>(60.0, 0.0)) == 0.0);  // phase starts at 0
        BOOST_TEST(meanPhaseStep(x, 0, x.size()) == 2.0 * kTestPi * cfo / 2.4e6, tt::tolerance(0.02));
    }
}

BOOST_AUTO_TEST_CASE(writer_counts_clipping)
{
    std::ostringstream os;
    IqWriter w(os, kModesRate, 32.15, std::nullopt, 0.0, 1);
    const std::vector<std::complex<float>> x(1000, {200.0f, 0.0f});
    w.write(x.data(), x.size());
    const std::string bytes = os.str();
    for (std::size_t k = 0; k < bytes.size(); k += 2) {
        BOOST_TEST(static_cast<uint8_t>(bytes[k]) == 255u);
        BOOST_TEST(static_cast<uint8_t>(bytes[k + 1]) == 128u);
    }
    BOOST_TEST(w.samples() == 1000u);
    BOOST_TEST(w.clipped() == w.samples());

    const std::vector<std::complex<float>> y(10, {-200.0f, 300.0f});  // both clip
    w.write(y.data(), y.size());
    BOOST_TEST(w.samples() == 1010u);
    BOOST_TEST(w.clipped() == 1020u);
    BOOST_TEST(static_cast<uint8_t>(os.str()[2000]) == 0u);
    BOOST_TEST(static_cast<uint8_t>(os.str()[2001]) == 255u);

    std::ostream broken(nullptr);  // every write fails
    IqWriter bad(broken, kModesRate, 32.15, std::nullopt, 0.0, 1);
    BOOST_CHECK_THROW(bad.write(x.data(), x.size()), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(resample_2M_to_2M4_count)
{
    std::istringstream in(std::string(2 * 2000000, static_cast<char>(128)));
    std::ostringstream out;
    const ResampleCounts n = resampleCu8(in, 2000000, 2400000, out);
    BOOST_TEST(n.in_samples == 2000000u);
    BOOST_TEST(n.out_samples >= 2400000u - 1);
    BOOST_TEST(n.out_samples <= 2400000u + 1);
    const std::string bytes = out.str();
    BOOST_TEST(bytes.size() == 2 * n.out_samples);
    BOOST_TEST(bytes.find_first_not_of(static_cast<char>(128)) == std::string::npos);  // zero stays zero
    BOOST_TEST(n.clipped == 0u);

    // A full-scale step rings (Gibbs) past 0..255: the overshoot is counted.
    std::string step(2 * 1000, static_cast<char>(0));
    step.append(2 * 1000, static_cast<char>(255));
    std::istringstream step_in(step);
    std::ostringstream step_out;
    BOOST_TEST(resampleCu8(step_in, 2000000, 2400000, step_out).clipped > 0u);

    std::istringstream none("");
    BOOST_CHECK_THROW(resampleCu8(none, 2083333, 2400000, out), std::invalid_argument);  // 2400000/2083333
    BOOST_CHECK_THROW(resampleCu8(none, 0, 2400000, out), std::invalid_argument);
    BOOST_CHECK_NO_THROW(resampleCu8(none, 2400000, 2000000, out));  // 5/6
}

BOOST_AUTO_TEST_CASE(resample_keeps_tone_frequency)
{
    const double in_rate = 2.0e6, f = 1.0e5;
    std::string tone;
    tone.reserve(4000000);
    for (int n = 0; n < 2000000; ++n) {
        const double ph = 2.0 * kTestPi * f * n / in_rate;
        tone.push_back(static_cast<char>(static_cast<uint8_t>(std::round(128.0 + 60.0 * std::cos(ph)))));
        tone.push_back(static_cast<char>(static_cast<uint8_t>(std::round(128.0 + 60.0 * std::sin(ph)))));
    }
    std::istringstream in(tone);
    std::ostringstream out;
    const ResampleCounts n = resampleCu8(in, 2000000, 2400000, out);
    BOOST_TEST(n.out_samples == 2400000u);
    const auto y = cu8Samples(out.str());
    BOOST_REQUIRE(y.size() == n.out_samples);
    const std::size_t from = y.size() / 20, to = y.size() - y.size() / 20;
    BOOST_TEST(meanPhaseStep(y, from, to) * 2.4e6 / (2.0 * kTestPi) == f, tt::tolerance(0.005));
    double mag = 0.0;
    for (std::size_t k = from; k < to; ++k)
        mag += std::abs(y[k]);
    BOOST_TEST(mag / static_cast<double>(to - from) == 60.0, tt::tolerance(0.02));  // passband gain 1
}

namespace {

// Mean output magnitude of a complex tone at f Hz (amplitude 60 counts, 0.5 s)
// resampled from in_rate to out_rate, over the middle 90 % of the output.
double resampledToneMagnitude(double f, uint64_t in_rate, uint64_t out_rate)
{
    const int count = static_cast<int>(in_rate / 2);
    std::string tone;
    tone.reserve(2 * static_cast<std::size_t>(count));
    for (int n = 0; n < count; ++n) {
        const double ph = 2.0 * kTestPi * f * n / static_cast<double>(in_rate);
        tone.push_back(static_cast<char>(static_cast<uint8_t>(std::round(128.0 + 60.0 * std::cos(ph)))));
        tone.push_back(static_cast<char>(static_cast<uint8_t>(std::round(128.0 + 60.0 * std::sin(ph)))));
    }
    std::istringstream in(tone);
    std::ostringstream out;
    resampleCu8(in, in_rate, out_rate, out);
    const auto y = cu8Samples(out.str());
    const std::size_t from = y.size() / 20, to = y.size() - y.size() / 20;
    double mag = 0.0;
    for (std::size_t k = from; k < to; ++k)
        mag += std::abs(y[k]);
    return mag / static_cast<double>(to - from);
}

}  // namespace

// The cutoff is min(0.5 * in_rate, 0.45 * out_rate): upsampling keeps the
// input's band up to its Nyquist frequency (a 2 MS/s capture's +-1 MHz, which
// readsb's 2.4 MS/s demodulator needs); downsampling still rejects what would alias.
BOOST_AUTO_TEST_CASE(resample_passband_reaches_input_nyquist_when_upsampling)
{
    const double up_db = 20.0 * std::log10(resampledToneMagnitude(0.47 * 2.0e6, 2000000, 2400000) / 60.0);
    BOOST_TEST(up_db >= -3.0);
    const double down_db = 20.0 * std::log10(resampledToneMagnitude(0.48 * 2.4e6, 2400000, 2000000) / 60.0);
    BOOST_TEST(down_db <= -20.0);
}

BOOST_AUTO_TEST_CASE(info_numbers)
{
    std::string bytes(2 * 24000, static_cast<char>(128));
    {
        std::istringstream in(bytes);
        const Cu8Info i = cu8Info(in, 2.4e6);
        BOOST_TEST(i.samples == 24000u);
        BOOST_TEST(i.seconds == 0.01, tt::tolerance(1e-12));
        BOOST_TEST(i.dc_i == 0.0);
        BOOST_TEST(i.dc_q == 0.0);
        BOOST_TEST(i.peak_mag == 0.0);
        BOOST_TEST(i.mean_mag == 0.0);
    }
    bytes.push_back(static_cast<char>(228));
    bytes.push_back(static_cast<char>(128));
    bytes.push_back(static_cast<char>(7));  // a trailing odd byte is ignored
    {
        std::istringstream in(bytes);
        const Cu8Info i = cu8Info(in, 2.4e6);
        BOOST_TEST(i.samples == 24001u);
        BOOST_TEST(i.peak_mag == 100.0);
        BOOST_TEST(i.mean_mag == 100.0 / 24001.0, tt::tolerance(1e-9));
        BOOST_TEST(i.dc_i == 100.0 / 24001.0, tt::tolerance(1e-9));
        BOOST_TEST(i.dc_q == 0.0);
    }
    {
        std::istringstream in("");
        const Cu8Info i = cu8Info(in, 2.4e6);
        BOOST_TEST(i.samples == 0u);
        BOOST_TEST(i.seconds == 0.0);
    }
    {
        // DC of a noisy constant over 100 000 samples, within +-1.0.
        std::istringstream in(writeConstant({10.0f, -5.0f}, 100000, 32.15, 10.0, 0.0, 3));
        const Cu8Info i = cu8Info(in, 2.4e6);
        BOOST_TEST(i.samples == 100000u);
        BOOST_TEST(std::fabs(i.dc_i - 10.0) <= 1.0);
        BOOST_TEST(std::fabs(i.dc_q + 5.0) <= 1.0);
    }
}

// ---- inputs and placement -----------------------------------------------------

namespace {

// Mode S placement as skycue-iqgen does it with --pad-ms 0.05 and
// --spacing-us 2000: pad 600 ticks, spacing 24000 ticks, times in ticks.
PlaceOptions modesOptions()
{
    PlaceOptions o;
    o.pad = 600;
    o.spacing = 24000;
    o.grid_per_time = 1.0;
    o.length = [](const std::vector<uint8_t>& p) { return static_cast<int64_t>(modesFrameTicks(p.size())); };
    return o;
}

// A 112-bit frame filled with tag, so frames can be told apart.
TimedFrame frameAt(std::optional<int64_t> t, uint8_t tag, std::size_t bytes = 14)
{
    return TimedFrame{t, std::vector<uint8_t>(bytes, tag), "test"};
}

std::vector<int64_t> starts(const Placement& p)
{
    std::vector<int64_t> out;
    for (const auto& f : p.frames)
        out.push_back(f.start);
    return out;
}

// A fresh directory, removed (with its files) at scope exit.
struct TempDir {
    std::string path;
    std::vector<std::string> files;
    TempDir()
    {
        const char* base = std::getenv("TMPDIR");
        std::string tmpl = std::string(base && *base ? base : "/tmp") + "/test_iqgen.XXXXXX";
        BOOST_REQUIRE(mkdtemp(&tmpl[0]) != nullptr);
        path = tmpl;
    }
    ~TempDir()
    {
        for (const auto& f : files)
            ::unlink(f.c_str());
        ::rmdir(path.c_str());
    }
    std::string write(const std::string& name, const std::string& bytes)
    {
        const std::string p = path + "/" + name;
        std::ofstream(p, std::ios::binary) << bytes;
        files.push_back(p);
        return p;
    }
};

// One Beast frame: 0x1a, type, 6-byte timestamp, signal, payload; 0x1a
// bytes in the body doubled.
std::string beastFrame(char type, uint64_t ts, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> body;
    for (int k = 5; k >= 0; --k)
        body.push_back(static_cast<uint8_t>(ts >> (8 * k)));
    body.push_back(0x80);  // signal
    body.insert(body.end(), payload.begin(), payload.end());
    std::string out{'\x1a', type};
    for (uint8_t b : body) {
        out += static_cast<char>(b);
        if (b == 0x1a)
            out += '\x1a';
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(frames_100us_apart_start_240_samples_apart)
{
    // 56-bit frames (768 ticks long), so 1200 ticks apart they do not overlap.
    const Placement p = place({{frameAt(0, 1, 7), frameAt(1200, 2, 7)}}, modesOptions());
    BOOST_REQUIRE(p.frames.size() == 2u);
    BOOST_TEST(p.frames[0].start == 600);  // shifted by pad
    BOOST_TEST(p.frames[1].start / 5 - p.frames[0].start / 5 == 240);
    BOOST_TEST(p.overlaps == 0u);
    BOOST_TEST(p.end == 1800 + 768 + 600);  // last frame end + pad
}

BOOST_AUTO_TEST_CASE(overlap_marks_both_and_counts_once)
{
    Placement p = place({{frameAt(0, 1), frameAt(700, 2)}}, modesOptions());
    BOOST_REQUIRE(p.frames.size() == 2u);
    BOOST_TEST(p.overlaps == 1u);
    BOOST_TEST(p.frames[0].overlap);
    BOOST_TEST(p.frames[1].overlap);

    p = place({{frameAt(0, 1), frameAt(1440, 2)}}, modesOptions());  // back to back: [0,1440) [1440,2880)
    BOOST_TEST(p.overlaps == 0u);
    BOOST_TEST(!p.frames[0].overlap);
    BOOST_TEST(!p.frames[1].overlap);

    // Three on top of each other: three pairs.
    p = place({{frameAt(0, 1), frameAt(10, 2), frameAt(20, 3, 7)}}, modesOptions());
    BOOST_TEST(p.overlaps == 3u);
}

BOOST_AUTO_TEST_CASE(identical_and_reordered_timestamps)
{
    // A at 5000, B and C both at 0: stable sort gives B, C, A; B and C overlap.
    const Placement p = place({{frameAt(5000, 'A'), frameAt(0, 'B'), frameAt(0, 'C')}}, modesOptions());
    BOOST_REQUIRE(p.frames.size() == 3u);
    BOOST_TEST(p.frames[0].payload.front() == 'B');
    BOOST_TEST(p.frames[1].payload.front() == 'C');
    BOOST_TEST(p.frames[2].payload.front() == 'A');
    BOOST_TEST(starts(p) == (std::vector<int64_t>{600, 600, 5600}), tt::per_element());
    BOOST_TEST(p.frames[0].overlap);
    BOOST_TEST(p.frames[1].overlap);
    BOOST_TEST(!p.frames[2].overlap);
    BOOST_TEST(p.overlaps == 1u);

    // The same frames in time order give the identical placement.
    const Placement q = place({{frameAt(0, 'B'), frameAt(0, 'C'), frameAt(5000, 'A')}}, modesOptions());
    BOOST_REQUIRE(q.frames.size() == p.frames.size());
    for (std::size_t i = 0; i < p.frames.size(); ++i) {
        BOOST_TEST(q.frames[i].start == p.frames[i].start);
        BOOST_TEST((q.frames[i].payload == p.frames[i].payload));
        BOOST_TEST(q.frames[i].overlap == p.frames[i].overlap);
    }
    BOOST_TEST(q.overlaps == p.overlaps);
    BOOST_TEST(q.end == p.end);
}

BOOST_AUTO_TEST_CASE(beast_counter_wrap)
{
    TempDir dir;
    const uint64_t wrap = uint64_t{1} << 48;
    const auto df17 = hex("8D4840D6202CC371C32CE0576098");
    std::vector<uint8_t> escaped = df17;
    escaped[3] = 0x1a;  // exercises the escape on the way in
    const std::string bin = dir.write(
        "wrap.bin", beastFrame('3', wrap - 1200, df17) + beastFrame('1', wrap - 600, {0x12, 0x34}) +
                        beastFrame('3', 1200, escaped));
    InputStats st;
    const auto frames = readModesFile(bin, st);
    BOOST_REQUIRE(frames.size() == 2u);
    BOOST_TEST(st.skipped == 1u);  // the Mode A/C frame
    BOOST_TEST((frames[0].payload == df17));
    BOOST_TEST((frames[1].payload == escaped));
    BOOST_TEST(frames[0].where.find("wrap.bin@0") != std::string::npos);
    Placement p = place({frames}, modesOptions());
    BOOST_REQUIRE(p.frames.size() == 2u);
    BOOST_TEST(p.frames[1].start - p.frames[0].start == 2400);
    BOOST_TEST(p.frames[1].start / 5 - p.frames[0].start / 5 == 480);

    // The AVR "@" counter is the same 12 MHz 48-bit clock: same unwrap.
    const std::string avr =
        dir.write("wrap.avr", "@FFFFFFFFFB508D4840D6202CC371C32CE0576098;\n@0000000004B08D4840D6202CC371C32CE0576098;\n");
    InputStats st2;
    const auto lines = readModesFile(avr, st2);
    BOOST_REQUIRE(lines.size() == 2u);
    BOOST_TEST(st2.skipped == 0u);
    BOOST_TEST(*lines[1].time - *lines[0].time == 2400);
    p = place({lines}, modesOptions());
    BOOST_REQUIRE(p.frames.size() == 2u);
    BOOST_TEST(p.frames[1].start / 5 - p.frames[0].start / 5 == 480);
    BOOST_TEST(lines[1].where == avr + ":2");
}

BOOST_AUTO_TEST_CASE(untimed_frames_use_spacing)
{
    const Placement p = place({{frameAt(std::nullopt, 1), frameAt(std::nullopt, 2), frameAt(std::nullopt, 3)}},
                              modesOptions());
    BOOST_TEST(starts(p) == (std::vector<int64_t>{600, 24600, 48600}), tt::per_element());
    BOOST_TEST(p.overlaps == 0u);
}

BOOST_AUTO_TEST_CASE(limit_and_duration)
{
    std::vector<TimedFrame> five;
    for (uint8_t k = 0; k < 5; ++k)
        five.push_back(frameAt(std::nullopt, k));
    PlaceOptions o = modesOptions();
    o.limit = 2;
    Placement p = place({five}, o);
    BOOST_TEST(p.frames.size() == 2u);
    BOOST_TEST(p.end == 24600 + 1440 + 600);

    o = modesOptions();
    o.duration = 30000;  // starts (before pad) 0 and 24000 are in, 48000 is not
    p = place({five}, o);
    BOOST_TEST(starts(p) == (std::vector<int64_t>{600, 24600}), tt::per_element());

    o.duration = 24000;  // start - pad >= duration is dropped
    BOOST_TEST(place({five}, o).frames.size() == 1u);
}

BOOST_AUTO_TEST_CASE(files_concatenate)
{
    // File 1 timed (last start 1200), file 2 timed far later, file 3 untimed.
    const Placement p = place({{frameAt(0, 1, 7), frameAt(1200, 2, 7)},
                               {frameAt(1000000000, 3), frameAt(1000000100, 4)},
                               {frameAt(std::nullopt, 5), frameAt(std::nullopt, 6)}},
                              modesOptions());
    BOOST_TEST(starts(p) == (std::vector<int64_t>{600, 1800, 25800, 25900, 49900, 73900}), tt::per_element());
    BOOST_TEST(p.frames[2].payload.front() == 3);
    BOOST_TEST(p.frames[4].payload.front() == 5);
    BOOST_TEST(p.frames[2].overlap);  // 3 and 4 are 100 ticks apart
    BOOST_TEST(p.overlaps == 1u);
    BOOST_TEST(place({}, modesOptions()).frames.empty());
    BOOST_TEST(place({{}, {}}, modesOptions()).end == 0);
}

BOOST_AUTO_TEST_CASE(readers_skip_count_and_reject)
{
    TempDir dir;
    // csv: a Mode A/C line and a malformed line are skipped and counted;
    // blank lines and comments are not counted.
    const std::string csv = dir.write("m.csv",
                                      "# capture\n\n1.0,5F406A936F50F0\n1.5,1234\nnot a line\n"
                                      "2.000001,8D4840D6202CC371C32CE0576098\r\n");
    InputStats st;
    const auto m = readModesFile(csv, st);
    BOOST_REQUIRE(m.size() == 2u);
    BOOST_TEST(st.skipped == 2u);
    BOOST_TEST(*m[0].time == 12000000);
    BOOST_TEST(*m[1].time == 24000012);
    BOOST_TEST(m[1].where == csv + ":6");

    // AVR: Mode A/C and "*" untimed.
    const std::string txt = dir.write("m.txt", "*5F406A936F50F0;\n@000000B71B001234;\n");
    InputStats st2;
    const auto a = readModesFile(txt, st2);
    BOOST_REQUIRE(a.size() == 1u);
    BOOST_TEST(!a[0].time);
    BOOST_TEST(st2.skipped == 1u);

    BOOST_CHECK_THROW(readModesFile(dir.path + "/in.xyz", st), InputError);
    BOOST_CHECK_THROW(readModesFile(dir.path + "/nope.csv", st), InputError);
    BOOST_CHECK_THROW(readUatFile(dir.path + "/u.csv", st), InputError);

    // UAT: payload, ms timestamps, and a bad length naming FILE:LINE.
    const std::string uat = dir.write(
        "u.txt", "1700000000123 -00a66ef135445d525a0c0519119021204800;rs=1;\n-00a66ef135445d525a0c0519119021204800\n");
    InputStats st3;
    const auto u = readUatFile(uat, st3);
    BOOST_REQUIRE(u.size() == 2u);
    BOOST_TEST(u[0].payload.size() == 18u);
    BOOST_TEST(*u[0].time == 1700000000123LL * 1000000);
    BOOST_TEST(!u[1].time);
    const std::string bad = dir.write("bad.txt", "-" + std::string(40, 'a') + ";\n");
    try {
        readUatFile(bad, st3);
        BOOST_FAIL("a 20-byte payload was accepted");
    } catch (const InputError& e) {
        BOOST_TEST(std::string(e.what()).find(bad + ":1") != std::string::npos, e.what());
    }
}

BOOST_AUTO_TEST_CASE(unwrap_overflow_is_an_input_error)
{
    // Every step is 2^47 - 1 ticks forward (the largest the signed 48-bit
    // delta allows), so the unwrapped time passes 2^63 after 65 536 lines.
    TempDir dir;
    std::string avr;
    const uint64_t step = (uint64_t{1} << 47) - 1;
    char line[64];
    for (uint64_t k = 0; k < 65540; ++k) {
        std::snprintf(line, sizeof line, "@%012llX5F406A936F50F0;\n",
                      static_cast<unsigned long long>((k * step) & ((uint64_t{1} << 48) - 1)));
        avr += line;
    }
    const std::string path = dir.write("far.avr", avr);
    InputStats st;
    try {
        readModesFile(path, st);
        BOOST_FAIL("an unwrapped time past 2^63 was accepted");
    } catch (const InputError& e) {
        BOOST_TEST(std::string(e.what()).find(path + ":") != std::string::npos, e.what());
    }
}
