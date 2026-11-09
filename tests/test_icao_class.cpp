#define BOOST_TEST_MODULE skycued_icao_class
#include <boost/test/unit_test.hpp>

#include "icao_class.hpp"
#include "json_lite.hpp"
#include "stats_report.hpp"
#include "time_util.hpp"

#include <skycue/types.hpp>

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace skycued;

namespace {

// The generated tables, read here the way icao_class.cpp reads them, to check
// their order.
struct IcaoBlock {
    uint32_t first;
    uint32_t last;
    const char* name;
};
struct MilitaryBlock {
    uint32_t first;
    uint32_t last;
};
#include "icao_tables.inc"

JsonObject loadSample()
{
    const std::string path = SKYCUE_TEST_DATA_DIR "/icao_class_cases.json";
    std::ifstream in(path, std::ios::binary);
    BOOST_TEST_REQUIRE(in.good(), "cannot open " << path);
    std::ostringstream ss;
    ss << in.rdbuf();
    auto obj = JsonObject::parse(ss.str(), 1024 * 1024);
    BOOST_TEST_REQUIRE(obj.has_value(), "cannot parse " << path);
    return std::move(*obj);
}

std::vector<JsonObject> group(const JsonObject& fx, const char* name)
{
    auto cases = fx.getObjectArray(name);
    BOOST_TEST_REQUIRE(cases.has_value(), "group " << name);
    BOOST_TEST_REQUIRE(!cases->empty(), "group " << name);
    return std::move(*cases);
}

std::string field(const JsonObject& c, const char* key) { return c.getString(key).value_or(""); }

// The sample's addr_type, as the API spells it; absent is icao, as in the JS.
skycue::AddressType addrType(const JsonObject& c)
{
    const std::string t = field(c, "addr_type");
    if (t.empty() || t == "icao")
        return skycue::AddressType::icao;
    if (t == "non_icao")
        return skycue::AddressType::non_icao;
    if (t == "tisb_trackfile")
        return skycue::AddressType::tisb_trackfile;
    BOOST_TEST_REQUIRE(t == "other", "unknown addr_type " << t);
    return skycue::AddressType::other;
}

// A case as text, for the failure message: its fields in file order.
std::string describe(const JsonObject& c)
{
    std::string out = "{";
    for (const auto& k : c.keys())
        out += k + "=" + field(c, k.c_str()) + " ";
    return out + "}";
}

std::string text(const std::optional<std::string>& s) { return s ? *s : "(null)"; }

const char* kindName(CallsignKind k)
{
    switch (k) {
    case CallsignKind::none: return "none";
    case CallsignKind::airline: return "airline";
    case CallsignKind::registration: return "registration";
    case CallsignKind::military: return "military";
    case CallsignKind::other: return "other";
    }
    return "?";
}

}  // namespace

BOOST_AUTO_TEST_CASE(sample_cases_match_js)
{
    const JsonObject fx = loadSample();

    for (const auto& c : group(fx, "country"))
        BOOST_TEST(text(icaoCountry(field(c, "hex"))) == text(c.getString("want")), describe(c));

    for (const auto& c : group(fx, "track_country"))
        BOOST_TEST(text(trackCountry(field(c, "hex"), addrType(c))) == text(c.getString("want")), describe(c));

    for (const auto& c : group(fx, "military")) {
        const auto want = c.getBool("want");
        BOOST_TEST_REQUIRE(want.has_value(), describe(c));
        BOOST_TEST(icaoMilitary(field(c, "hex")) == *want, describe(c));
    }

    for (const auto& c : group(fx, "callsign_kind"))
        BOOST_TEST(std::string(kindName(callsignKind(field(c, "callsign")))) ==
                       c.getString("want").value_or("none"),
                   describe(c));

    for (const auto& c : group(fx, "track_class")) {
        ClassInput in;
        in.hex = field(c, "hex");
        in.addr_type = addrType(c);
        in.callsign = field(c, "callsign");
        in.category = field(c, "category");
        in.registration = field(c, "registration");
        BOOST_TEST(std::string(trackClass(in)) == field(c, "want"), describe(c));
    }
}

BOOST_AUTO_TEST_CASE(class_ignores_non_icao_digits)
{
    const skycue::TrackId tisb{0xae1234, skycue::AddressType::tisb_trackfile};
    const std::string hex = trackIdText(tisb);
    BOOST_TEST(hex == "~ae1234");
    BOOST_TEST(!icaoCountry(hex).has_value());
    BOOST_TEST(!icaoMilitary(hex));
    BOOST_TEST(!trackCountry(hexAddress(tisb.address), tisb.type).has_value());
    BOOST_TEST(text(trackCountry(hexAddress(0xae1234), skycue::AddressType::icao)) ==
               "United States");

    ClassInput in;
    in.hex = hex;
    in.addr_type = tisb.type;
    BOOST_TEST(std::string(trackClass(in)) == "unknown");
    in.hex = "ae1234";  // the bare digits are still not an ICAO address
    BOOST_TEST(std::string(trackClass(in)) == "unknown");
    in.addr_type = skycue::AddressType::icao;
    BOOST_TEST(std::string(trackClass(in)) == "military");
}

BOOST_AUTO_TEST_CASE(tables_sorted_and_disjoint)
{
    const std::size_t n_icao = sizeof kIcaoBlocks / sizeof kIcaoBlocks[0];
    const std::size_t n_mil = sizeof kMilitaryBlocks / sizeof kMilitaryBlocks[0];
    BOOST_TEST(n_icao > 150u);
    BOOST_TEST(n_mil > 10u);
    for (std::size_t i = 0; i < n_icao; ++i) {
        BOOST_TEST(kIcaoBlocks[i].first <= kIcaoBlocks[i].last, "icao block " << i);
        BOOST_TEST(kIcaoBlocks[i].last <= 0xFFFFFFu, "icao block " << i);
        if (i > 0)
            BOOST_TEST(kIcaoBlocks[i - 1].last < kIcaoBlocks[i].first, "icao block " << i);
    }
    for (std::size_t i = 0; i < n_mil; ++i) {
        BOOST_TEST(kMilitaryBlocks[i].first <= kMilitaryBlocks[i].last, "military block " << i);
        BOOST_TEST(kMilitaryBlocks[i].last <= 0xFFFFFFu, "military block " << i);
        if (i > 0)
            BOOST_TEST(kMilitaryBlocks[i - 1].last < kMilitaryBlocks[i].first, "military block " << i);
    }
    BOOST_TEST(sizeof kMilitaryCallsignPrefixes / sizeof kMilitaryCallsignPrefixes[0] > 0u);
    BOOST_TEST(std::string(kMilitaryCallsignPrefixes[0]) == "RCH");
}
