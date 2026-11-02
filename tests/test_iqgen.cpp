#define BOOST_TEST_MODULE iqgen
#include <boost/test/unit_test.hpp>

#include "gz_line_reader.hpp"

#include "bits.hpp"
#include "modes_crc.hpp"
#include "reed_solomon.hpp"
#include "uat_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace iqgen;
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
BOOST_AUTO_TEST_CASE(rs_every_fixture_codeword_has_zero_syndromes)
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
BOOST_AUTO_TEST_CASE(rs_corrupted_codeword_has_nonzero_syndrome)
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
BOOST_AUTO_TEST_CASE(uat_sync_word_first)
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

BOOST_AUTO_TEST_CASE(uplink_interleave)
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
