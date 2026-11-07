#define BOOST_TEST_MODULE coding
#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "coding/bits.hpp"
#include "coding/modes_crc.hpp"

using namespace skycued::coding;

namespace {

std::vector<uint8_t> hex(const std::string& s)
{
    std::vector<uint8_t> out;
    BOOST_REQUIRE(parseHex(s, out));
    return out;
}

const char* const kFrameA = "8D4840D6202CC371C32CE0576098";
const char* const kFrameB = "8D40621D58C382D690C8AC2863A7";

void flip(std::vector<uint8_t>& f, unsigned bit)
{
    f[bit / 8] ^= static_cast<uint8_t>(0x80u >> (bit % 8));
}

}  // namespace

BOOST_AUTO_TEST_CASE(residual_zero_for_clean_frames)
{
    BOOST_TEST(modesCrcResidual(hex(kFrameA)) == 0u);
    BOOST_TEST(modesCrcResidual(hex(kFrameB)) == 0u);
}

BOOST_AUTO_TEST_CASE(syndrome_fix_every_bit_outside_df)
{
    for (const char* h : {kFrameA, kFrameB}) {
        const auto clean = hex(h);
        for (unsigned bit = 5; bit < 112; ++bit) {
            auto f = clean;
            flip(f, bit);
            BOOST_TEST_CONTEXT("frame " << h << " bit " << bit)
            {
                BOOST_TEST(syndromeFix1(f));
                BOOST_TEST(f == clean);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(syndrome_fix_refuses_df_bits)
{
    // A flip in the five DF bits changes what the frame is, so it stays
    // uncorrected by spec section 5.2 even when the syndrome matches.
    const auto clean = hex(kFrameA);
    for (unsigned bit = 0; bit < 5; ++bit) {
        auto f = clean;
        flip(f, bit);
        const auto flipped = f;
        BOOST_TEST_CONTEXT("bit " << bit)
        {
            BOOST_TEST(!syndromeFix1(f));
            BOOST_TEST(f == flipped);
        }
    }
}

BOOST_AUTO_TEST_CASE(syndrome_fix_refuses_two_bits)
{
    auto f = hex(kFrameA);
    flip(f, 20);
    flip(f, 70);
    const auto before = f;
    BOOST_TEST(!syndromeFix1(f));
    BOOST_TEST(f == before);
}

BOOST_AUTO_TEST_CASE(syndrome_fix_refuses_short_frames)
{
    auto f = hex("5D486258DD7987");  // DF11, 7 bytes, clean
    flip(f, 30);
    const auto before = f;
    BOOST_TEST(!syndromeFix1(f));
    BOOST_TEST(f == before);
}
