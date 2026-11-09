#define BOOST_TEST_MODULE skycued_track_names
#include <boost/test/unit_test.hpp>

#include "recorder.hpp"

#include <skycue/types.hpp>

#include <string>

// The enum texts are part of the recording file format and the stats files:
// these tests pin them.

using namespace skycued::recording;
using skycue::AddressType;
using skycue::DropReason;
using skycue::Source;

BOOST_AUTO_TEST_CASE(address_type_names_are_frozen)
{
    BOOST_TEST(std::string(addressTypeName(AddressType::icao)) == "icao");
    BOOST_TEST(std::string(addressTypeName(AddressType::non_icao)) == "non_icao");
    BOOST_TEST(std::string(addressTypeName(AddressType::tisb_trackfile)) == "tisb_trackfile");
    BOOST_TEST(std::string(addressTypeName(AddressType::other)) == "other");
}

BOOST_AUTO_TEST_CASE(source_names_are_frozen)
{
    BOOST_TEST(std::string(sourceName(Source::adsb_1090)) == "adsb_1090");
    BOOST_TEST(std::string(sourceName(Source::uat_978)) == "uat_978");
    BOOST_TEST(std::string(sourceName(Source::tisb)) == "tisb");
    BOOST_TEST(std::string(sourceName(Source::adsr)) == "adsr");
    BOOST_TEST(std::string(sourceName(Source::mlat)) == "mlat");
    BOOST_TEST(std::string(sourceName(Source::other)) == "other");
}

BOOST_AUTO_TEST_CASE(drop_reason_names_are_frozen)
{
    BOOST_TEST(std::string(dropReasonName(DropReason::timeout)) == "timeout");
    BOOST_TEST(std::string(dropReasonName(DropReason::filtered)) == "filtered");
    BOOST_TEST(std::string(dropReasonName(DropReason::source_lost)) == "source_lost");
    BOOST_TEST(std::string(dropReasonName(DropReason::shutdown)) == "shutdown");
    BOOST_TEST(std::string(dropReasonName(DropReason::stale)) == "stale");
}

BOOST_AUTO_TEST_CASE(names_parse_back)
{
    for (auto t : {AddressType::icao, AddressType::non_icao, AddressType::tisb_trackfile, AddressType::other}) {
        auto back = parseAddressType(addressTypeName(t));
        BOOST_REQUIRE(back.has_value());
        BOOST_TEST((*back == t));
    }
    for (auto s : {Source::adsb_1090, Source::uat_978, Source::tisb, Source::adsr, Source::mlat, Source::other}) {
        auto back = parseSource(sourceName(s));
        BOOST_REQUIRE(back.has_value());
        BOOST_TEST((*back == s));
    }
    for (auto r : {DropReason::timeout, DropReason::filtered, DropReason::source_lost, DropReason::shutdown,
                   DropReason::stale}) {
        auto back = parseDropReason(dropReasonName(r));
        BOOST_REQUIRE(back.has_value());
        BOOST_TEST((*back == r));
    }
    for (const char* bad : {"", "bogus", "ICAO", "icao "}) {
        BOOST_TEST(!parseAddressType(bad).has_value());
        BOOST_TEST(!parseSource(bad).has_value());
        BOOST_TEST(!parseDropReason(bad).has_value());
    }
}

BOOST_AUTO_TEST_CASE(out_of_range_values_use_the_recorder_fallbacks)
{
    BOOST_TEST(std::string(addressTypeName(static_cast<AddressType>(200))) == "other");
    BOOST_TEST(std::string(sourceName(static_cast<Source>(200))) == "other");
    BOOST_TEST(std::string(dropReasonName(static_cast<DropReason>(200))) == "timeout");
}
