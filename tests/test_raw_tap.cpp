#define BOOST_TEST_MODULE skycued_raw_tap
#include <boost/test/unit_test.hpp>

#include "hub/raw_tap.hpp"

#include <chrono>
#include <string>
#include <vector>

using namespace skycued;

namespace {

struct Seen {
    RawKind kind;
    std::vector<uint8_t> bytes;
    std::string input;
    std::chrono::system_clock::time_point arrived;
};

}  // namespace

BOOST_AUTO_TEST_CASE(unwanted_kind_builds_no_buffer)
{
    RawTap tap;
    BOOST_TEST(!tap.wanted(RawKind::beast));  // no sink
    const uint8_t frame[] = {0x1a, 0x32, 0x00};
    tap.publish(RawKind::beast, frame, sizeof frame, "in");  // nothing to deliver to: no crash

    int calls = 0, asked = 0;
    tap.setSink([&](const RawMessage&) { ++calls; }, [&](RawKind k) {
        ++asked;
        return k == RawKind::uat;
    });
    BOOST_TEST(!tap.wanted(RawKind::beast));
    BOOST_TEST(tap.wanted(RawKind::uat));
    tap.publish(RawKind::beast, frame, sizeof frame, "in");
    BOOST_TEST(calls == 0);
    BOOST_TEST(asked >= 1);
}

BOOST_AUTO_TEST_CASE(wanted_kind_delivers_copy_with_input_and_time)
{
    RawTap tap;
    std::vector<Seen> seen;
    tap.setSink(
        [&](const RawMessage& m) {
            BOOST_REQUIRE(m.bytes);
            seen.push_back({m.kind, *m.bytes, std::string(m.input), m.arrived});
        },
        [](RawKind k) { return k == RawKind::beast || k == RawKind::cat021; });
    uint8_t frame[] = {0x1a, 0x32, 0x01, 0x02};
    const auto before = std::chrono::system_clock::now();
    tap.publish(RawKind::beast, frame, sizeof frame, "rx-1");
    const auto after = std::chrono::system_clock::now();
    frame[2] = 0xff;  // the delivered bytes are a copy
    tap.publish(RawKind::uat, frame, sizeof frame, "rx-2");  // not wanted
    BOOST_REQUIRE(seen.size() == 1u);
    BOOST_TEST((seen[0].kind == RawKind::beast));
    BOOST_TEST((seen[0].bytes == std::vector<uint8_t>{0x1a, 0x32, 0x01, 0x02}));
    BOOST_TEST(seen[0].input == "rx-1");
    BOOST_TEST((seen[0].arrived >= before && seen[0].arrived <= after));
    // An empty Wanted wants every kind.
    int n = 0;
    tap.setSink([&](const RawMessage&) { ++n; }, nullptr);
    BOOST_TEST(tap.wanted(RawKind::json));
    tap.publish(RawKind::json, frame, sizeof frame, "x");
    BOOST_TEST(n == 1);
}

BOOST_AUTO_TEST_CASE(clear_stops_delivery)
{
    RawTap tap;
    int n = 0;
    tap.setSink([&](const RawMessage&) { ++n; }, [](RawKind) { return true; });
    const uint8_t b[] = {1};
    tap.publish(RawKind::json, b, 1, "x");
    BOOST_TEST(n == 1);
    tap.clear();
    BOOST_TEST(!tap.wanted(RawKind::json));
    tap.publish(RawKind::json, b, 1, "x");
    BOOST_TEST(n == 1);
}

BOOST_AUTO_TEST_CASE(kind_names)
{
    BOOST_TEST(std::string(rawKindName(RawKind::beast)) == "beast");
    BOOST_TEST(std::string(rawKindName(RawKind::uat)) == "uat");
    BOOST_TEST(std::string(rawKindName(RawKind::json)) == "json");
    BOOST_TEST(std::string(rawKindName(RawKind::pingstation_json)) == "pingstation_json");
    BOOST_TEST(std::string(rawKindName(RawKind::cat021)) == "cat021");
}
