// Beast and AVR frames as decoder input (frame decoder design, 5.1 and 8.1).
#define BOOST_TEST_MODULE decode_frame_source
#include <boost/test/unit_test.hpp>

#include <string>

#include "decode/frame_source.hpp"
#include "raw_formats.hpp"

using namespace skycued;
using namespace skycued::decode;

namespace {

beast::Frame beastFrame(uint8_t type, uint64_t ts, uint8_t signal, std::size_t payload) {
    beast::Frame f;
    f.type = type;
    f.timestamp = ts;
    f.body.assign(6, '\0');
    f.body.push_back(static_cast<char>(signal));
    for (std::size_t i = 0; i < payload; ++i) f.body.push_back(static_cast<char>(0x10 + i));
    return f;
}

constexpr uint64_t kMlat = 0xFF004D4C4154ULL;
const std::string kLong = "8D4840D6202CC371C32CE0576098";

}  // namespace

BOOST_AUTO_TEST_CASE(beast_type2_and_3_pass_with_signal) {
    FrameSourceCounters c;
    auto s = fromBeast(beastFrame('2', 1000, 0x55, 7), c);
    BOOST_REQUIRE(s);
    BOOST_CHECK_EQUAL(s->bytes.size(), 7u);
    BOOST_CHECK_EQUAL(s->bytes[0], 0x10);
    BOOST_REQUIRE(s->signal);
    BOOST_CHECK_EQUAL(*s->signal, 0x55);
    auto l = fromBeast(beastFrame('3', 1000, 0x66, 14), c);
    BOOST_REQUIRE(l);
    BOOST_CHECK_EQUAL(l->bytes.size(), 14u);
    BOOST_CHECK_EQUAL(*l->signal, 0x66);
    BOOST_CHECK_EQUAL(c.other_skipped + c.modeac_skipped + c.mlat_skipped + c.bad_avr, 0u);
}

BOOST_AUTO_TEST_CASE(beast_type5_treated_as_3) {
    FrameSourceCounters c;
    auto l = fromBeast(beastFrame('5', 1000, 0x42, 14), c);
    BOOST_REQUIRE(l);
    BOOST_CHECK_EQUAL(l->bytes.size(), 14u);
    BOOST_CHECK_EQUAL(*l->signal, 0x42);
}

BOOST_AUTO_TEST_CASE(beast_type1_counted_modeac) {
    FrameSourceCounters c;
    BOOST_CHECK(!fromBeast(beastFrame('1', 1000, 1, 2), c));
    BOOST_CHECK_EQUAL(c.modeac_skipped, 1u);
    BOOST_CHECK(!fromBeast(beastFrame('4', 1000, 1, 14), c));
    beast::Frame id;
    id.type = 0xe3;
    id.body.assign(8, '\0');
    BOOST_CHECK(!fromBeast(id, c));
    BOOST_CHECK_EQUAL(c.other_skipped, 2u);
    BOOST_CHECK_EQUAL(c.modeac_skipped, 1u);
}

BOOST_AUTO_TEST_CASE(beast_mlat_marker_skipped) {
    FrameSourceCounters c;
    BOOST_CHECK(!fromBeast(beastFrame('3', kMlat, 0, 14), c));
    BOOST_CHECK_EQUAL(c.mlat_skipped, 1u);
    BOOST_CHECK(fromBeast(beastFrame('3', kMlat + 1, 0, 14), c));
    BOOST_CHECK_EQUAL(c.mlat_skipped, 1u);
}

BOOST_AUTO_TEST_CASE(avr_star_and_at_forms) {
    FrameSourceCounters c;
    auto a = fromAvrLine("*" + kLong + ";\r\n", c);
    BOOST_REQUIRE(a);
    BOOST_CHECK_EQUAL(a->bytes.size(), 14u);
    BOOST_CHECK_EQUAL(a->bytes[0], 0x8D);
    BOOST_CHECK(!a->signal);
    auto b = fromAvrLine("@0000000003E8" + kLong + ";", c);
    BOOST_REQUIRE(b);
    BOOST_CHECK(b->bytes == a->bytes);
    auto s = fromAvrLine("*02E19718B0D2C7;", c);
    BOOST_REQUIRE(s);
    BOOST_CHECK_EQUAL(s->bytes.size(), 7u);
    BOOST_CHECK_EQUAL(c.bad_avr, 0u);
}

BOOST_AUTO_TEST_CASE(avr_bad_hex_counted) {
    FrameSourceCounters c;
    BOOST_CHECK(!fromAvrLine("*ZZ;", c));
    BOOST_CHECK(!fromAvrLine("", c));
    BOOST_CHECK(!fromAvrLine("*8D4840D6;", c));
    BOOST_CHECK_EQUAL(c.bad_avr, 3u);
}
