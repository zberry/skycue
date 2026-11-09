#define BOOST_TEST_MODULE skycued_readsb_json
#include <boost/test/unit_test.hpp>

#include "json_lite.hpp"
#include "observation.hpp"
#include "readsb_json_source.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

using skycue::AddressType;
using skycue::Source;
using skycued::JsonObject;
using skycued::JsonWriter;
using skycued::parseReadsbLine;

namespace {

// Seconds since the epoch, as a double, for comparing with readsb "now".
double toSeconds(std::chrono::system_clock::time_point t)
{
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

const char* const kMinimal =
    R"({"now":1793545200.250,"hex":"a4f2c1","type":"adsb_icao","flight":"UAL1423 ","r":"N37502",)"
    R"("alt_baro":36000,"alt_geom":36525,"gs":460.2,"track":270.1,"baro_rate":-64,"geom_rate":0,)"
    R"("squawk":"3341","emergency":"none","category":"A3","lat":38.912345,"lon":-75.987654,"nic":8,)"
    R"("rc":186,"seen_pos":0.5,"version":2,"nac_p":9,"mlat":[],"tisb":[],"messages":1234,"seen":0.2,)"
    R"("rssi":-12.3})";

// "7b22..." -> the bytes, for fuzz findings kept inline as hex.
std::string fromHex(const std::string& hex)
{
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(parses_minimal_line)
{
    auto obs = parseReadsbLine(kMinimal);
    BOOST_REQUIRE(obs);
    BOOST_TEST(obs->id.address == 0xa4f2c1u);
    BOOST_TEST(static_cast<int>(obs->id.type) == static_cast<int>(AddressType::icao));
    BOOST_TEST(static_cast<int>(obs->source) == static_cast<int>(Source::adsb_1090));
    BOOST_TEST(toSeconds(obs->received_time) == 1793545200.250, boost::test_tools::tolerance(1e-9));
    BOOST_REQUIRE(obs->position_time);
    BOOST_TEST(toSeconds(*obs->position_time) == 1793545199.750, boost::test_tools::tolerance(1e-9));
    BOOST_REQUIRE(obs->lat && obs->lon);
    BOOST_TEST(*obs->lat == 38.912345);
    BOOST_TEST(*obs->lon == -75.987654);
    BOOST_TEST((obs->on_ground == false));  // a numeric alt_baro asserts airborne
    BOOST_TEST(obs->alt_baro_ft.value_or(0) == 36000);
    BOOST_TEST(obs->alt_geom_ft.value_or(0) == 36525);
    BOOST_TEST(obs->gs_kt.value_or(0) == 460.2f);
    BOOST_TEST(obs->track_deg.value_or(0) == 270.1f);
    BOOST_TEST(obs->baro_rate_fpm.value_or(0) == -64.0f);
    BOOST_TEST(obs->geom_rate_fpm.value_or(1) == 0.0f);
    BOOST_TEST(obs->callsign.value_or("") == "UAL1423");  // trailing space trimmed
    BOOST_TEST(obs->squawk.value_or("") == "3341");
    BOOST_TEST(obs->registration.value_or("") == "N37502");
    BOOST_TEST(obs->emergency.value_or("") == "none");
    BOOST_TEST(obs->category.value_or("") == "A3");
    BOOST_TEST(obs->nic.value_or(0) == 8);
    BOOST_TEST(obs->nac_p.value_or(0) == 9);
    BOOST_TEST(obs->rssi.value_or(0) == -12.3);
    BOOST_TEST(obs->raw_line == kMinimal);
}

BOOST_AUTO_TEST_CASE(alt_geom_and_baro_set_diff)
{
    auto obs = parseReadsbLine(kMinimal);
    BOOST_REQUIRE(obs);
    BOOST_TEST(obs->geom_minus_baro_ft.value_or(0) == 525);  // 36525 - 36000
    BOOST_TEST(!obs->alt_baro_time);
    BOOST_TEST(!obs->alt_geom_time);
    auto neg = parseReadsbLine(R"({"now":5,"hex":"abcdef","type":"adsb_icao","alt_baro":36000,"alt_geom":35900})");
    BOOST_REQUIRE(neg);
    BOOST_TEST(neg->geom_minus_baro_ft.value_or(0) == -100);
}

BOOST_AUTO_TEST_CASE(no_diff_when_one_missing)
{
    auto baro = parseReadsbLine(R"({"now":5,"hex":"abcdef","type":"adsb_icao","alt_baro":36000})");
    BOOST_REQUIRE(baro);
    BOOST_TEST(!baro->geom_minus_baro_ft);
    auto geom = parseReadsbLine(R"({"now":5,"hex":"abcdef","type":"adsb_icao","alt_geom":36525})");
    BOOST_REQUIRE(geom);
    BOOST_TEST(!geom->geom_minus_baro_ft);
    auto ground = parseReadsbLine(R"({"now":5,"hex":"abcdef","type":"adsb_icao","alt_baro":"ground","alt_geom":100})");
    BOOST_REQUIRE(ground);
    BOOST_TEST(!ground->geom_minus_baro_ft);
}

BOOST_AUTO_TEST_CASE(tisb_other_has_no_geom)
{
    auto obs = parseReadsbLine(R"({"now":5,"hex":"abcdef","type":"tisb_other","alt_baro":36000})");
    BOOST_REQUIRE(obs);
    BOOST_TEST(!obs->alt_geom_ft);
    BOOST_TEST(!obs->geom_minus_baro_ft);
}

BOOST_AUTO_TEST_CASE(ground_altitude)
{
    auto obs = parseReadsbLine(
        R"({"now":1793545200.0,"hex":"a7d3e6","type":"adsb_icao","flight":"UAL455  ","alt_baro":"ground",)"
        R"("gs":14.6,"true_heading":181.4,"lat":38.85,"lon":-77.04,"nic":8,"rc":186,"seen_pos":0.1})");
    BOOST_REQUIRE(obs);
    BOOST_TEST((obs->on_ground == true));
    BOOST_TEST(!obs->alt_baro_ft);
    BOOST_TEST(!obs->alt_geom_ft);
    BOOST_TEST(obs->gs_kt.value_or(0) == 14.6f);
    BOOST_TEST(obs->lat.has_value());
}

BOOST_AUTO_TEST_CASE(on_ground_unset_without_alt_baro)
{
    // No alt_baro: the line says nothing about whether the target is on the ground.
    auto obs = parseReadsbLine(R"({"now":1793545200.0,"hex":"a7d3e6","type":"adsb_icao","lat":38.85,"lon":-77.04})");
    BOOST_REQUIRE(obs);
    BOOST_TEST(!obs->on_ground.has_value());
    BOOST_TEST(!obs->alt_baro_ft);
}

BOOST_AUTO_TEST_CASE(no_position_line)
{
    auto obs = parseReadsbLine(
        R"({"now":1793545210.5,"hex":"a2f7b9","type":"adsb_icao","flight":"FDX1520 ","alt_baro":24000,)"
        R"("gs":410,"track":92,"version":2,"messages":55,"seen":0.4,"rssi":-18.9})");
    BOOST_REQUIRE(obs);
    BOOST_TEST(!obs->lat);
    BOOST_TEST(!obs->lon);
    BOOST_TEST(!obs->position_time);
    BOOST_TEST(obs->alt_baro_ft.value_or(0) == 24000);
    BOOST_TEST(toSeconds(obs->received_time) == 1793545210.5, boost::test_tools::tolerance(1e-9));

    // lat without lon is not a position.
    auto half = parseReadsbLine(R"({"now":1,"hex":"a2f7b9","type":"adsb_icao","lat":38.9})");
    BOOST_REQUIRE(half);
    BOOST_TEST(!half->lat);
    BOOST_TEST(!half->position_time);

    // A position without seen_pos is taken as current.
    auto noseen = parseReadsbLine(R"({"now":100.5,"hex":"a2f7b9","type":"adsb_icao","lat":38.9,"lon":-77})");
    BOOST_REQUIRE(noseen && noseen->position_time);
    BOOST_TEST(toSeconds(*noseen->position_time) == 100.5, boost::test_tools::tolerance(1e-9));
}

BOOST_AUTO_TEST_CASE(unknown_key_ignored)
{
    auto obs = parseReadsbLine(
        R"({"now":5,"hex":"a4f2c1","type":"adsb_icao","zz_future_key":"x","zz_obj":{"k":[1,2,{"q":3}]},)"
        R"("lastPosition":{"lat":38.1,"lon":-77.2,"nic":8,"rc":186,"seen_pos":12.3},"alt_baro":1000})");
    BOOST_REQUIRE(obs);
    BOOST_TEST(obs->alt_baro_ft.value_or(0) == 1000);
    BOOST_TEST(!obs->lat);  // lastPosition is not a current position
}

BOOST_AUTO_TEST_CASE(tilde_hex_is_non_icao)
{
    auto obs = parseReadsbLine(R"({"now":5,"hex":"~2c4a1f","type":"tisb_other","lat":38.95,"lon":-77.1})");
    BOOST_REQUIRE(obs);
    BOOST_TEST(obs->id.address == 0x2c4a1fu);
    BOOST_TEST(static_cast<int>(obs->id.type) == static_cast<int>(AddressType::non_icao));
    BOOST_TEST(static_cast<int>(obs->source) == static_cast<int>(Source::tisb));

    BOOST_TEST(!parseReadsbLine(R"({"now":5,"hex":"~","type":"tisb_other"})"));
    BOOST_TEST(!parseReadsbLine(R"({"now":5,"hex":"zz1234","type":"adsb_icao"})"));
    BOOST_TEST(!parseReadsbLine(R"({"now":5,"hex":"a4f2c1ff","type":"adsb_icao"})"));
    BOOST_TEST(!parseReadsbLine(R"({"now":5,"type":"adsb_icao"})"));
}

BOOST_AUTO_TEST_CASE(type_mapping)
{
    struct Row {
        const char* type;
        Source source;
        AddressType address_type;
    };
    const Row rows[] = {
        {"adsb_icao", Source::adsb_1090, AddressType::icao},
        {"adsb_icao_nt", Source::adsb_1090, AddressType::icao},
        {"adsb_other", Source::adsb_1090, AddressType::icao},
        {"adsr_icao", Source::uat_978, AddressType::icao},
        {"adsr_other", Source::uat_978, AddressType::icao},
        {"tisb_icao", Source::tisb, AddressType::icao},
        {"tisb_other", Source::tisb, AddressType::icao},
        {"tisb_trackfile", Source::tisb, AddressType::tisb_trackfile},
        {"mlat", Source::mlat, AddressType::icao},
        {"mode_s", Source::other, AddressType::icao},
        {"other", Source::other, AddressType::icao},
        {"adsc", Source::other, AddressType::icao},
        {"unknown", Source::other, AddressType::icao},
        {"something_new", Source::other, AddressType::icao},
    };
    for (const auto& row : rows) {
        BOOST_TEST_CONTEXT(row.type)
        {
            auto obs = parseReadsbLine(std::string(R"({"now":5,"hex":"abcdef","type":")") + row.type + "\"}");
            BOOST_REQUIRE(obs);
            BOOST_TEST(static_cast<int>(obs->source) == static_cast<int>(row.source));
            BOOST_TEST(static_cast<int>(obs->id.type) == static_cast<int>(row.address_type));
        }
    }
    // A tisb_trackfile hex carries the ~ too; the track-file type wins.
    auto tf = parseReadsbLine(R"({"now":5,"hex":"~abcdef","type":"tisb_trackfile"})");
    BOOST_REQUIRE(tf);
    BOOST_TEST(static_cast<int>(tf->id.type) == static_cast<int>(AddressType::tisb_trackfile));
    // No type at all: other.
    auto none = parseReadsbLine(R"({"now":5,"hex":"abcdef"})");
    BOOST_REQUIRE(none);
    BOOST_TEST(static_cast<int>(none->source) == static_cast<int>(Source::other));
}

BOOST_AUTO_TEST_CASE(invalid_json_returns_nullopt)
{
    BOOST_TEST(!JsonObject::parse(""));
    BOOST_TEST(!JsonObject::parse("{"));
    BOOST_TEST(!JsonObject::parse(R"({"now":1,"hex":"abcdef",})"));
    BOOST_TEST(!JsonObject::parse("not json"));
    BOOST_TEST(!JsonObject::parse("[1,2,3]"));
    BOOST_TEST(!JsonObject::parse("42"));
    BOOST_TEST(!parseReadsbLine(""));
    BOOST_TEST(!parseReadsbLine(R"({"now":1,"hex":"abcdef")"));
    BOOST_TEST(!parseReadsbLine("[]"));
    // "now" is required and must be numeric.
    BOOST_TEST(!parseReadsbLine(R"({"hex":"abcdef","type":"adsb_icao"})"));
    BOOST_TEST(!parseReadsbLine(R"({"now":"soon","hex":"abcdef","type":"adsb_icao"})"));

    // A long garbage line is rejected, and quickly.
    auto start = std::chrono::steady_clock::now();
    BOOST_TEST(!parseReadsbLine(std::string(70 * 1024, 'a')));
    std::string long_valid = R"({"now":1,"hex":"abcdef","pad":")" + std::string(70 * 1024, 'a') + "\"}";
    BOOST_TEST(!parseReadsbLine(long_valid));
    const bool fast = std::chrono::steady_clock::now() - start < std::chrono::seconds(1);
    BOOST_TEST(fast);
}

BOOST_AUTO_TEST_CASE(nested_json_rejected_not_crash)
{
    std::optional<skycued::TrackObservation> obs;
    BOOST_CHECK_NO_THROW(obs = parseReadsbLine(R"({"a":{"b":1}})"));
    BOOST_TEST((!obs || !obs->lat));

    // Arrays and objects where scalars are expected are not read as scalars.
    BOOST_CHECK_NO_THROW(obs = parseReadsbLine(
                             R"({"now":5,"hex":"abcdef","type":"adsb_icao","flight":["x"],)"
                             R"("alt_baro":{"v":1},"lat":[38.9],"lon":-77,"gs":"fast"})"));
    BOOST_REQUIRE(obs);
    BOOST_TEST(!obs->callsign);
    BOOST_TEST(!obs->alt_baro_ft);
    BOOST_TEST(!obs->lat);
    BOOST_TEST(!obs->gs_kt);

    auto o = JsonObject::parse(R"({"a":{"b":1},"arr":[1,2],"s":"str","n":-3.5e2,"i":42,"g":"ground"})");
    BOOST_REQUIRE(o);
    BOOST_TEST(o->has("a"));
    BOOST_TEST(!o->has("b"));
    BOOST_TEST(!o->getString("a"));
    BOOST_TEST(!o->getDouble("arr"));
    BOOST_TEST(o->getString("s").value_or("") == "str");
    BOOST_TEST(o->getDouble("n").value_or(0) == -350.0);
    BOOST_TEST(o->getInt("i").value_or(0) == 42);
    BOOST_TEST(!o->getInt("n"));
    BOOST_TEST(!o->getDouble("g"));
    BOOST_TEST(o->getString("g").value_or("") == "ground");
    BOOST_TEST(!o->getDouble("missing"));
}

BOOST_AUTO_TEST_CASE(deep_array_nesting_rejected)
{
    auto start = std::chrono::steady_clock::now();
    const std::string deep(60000, '[');
    BOOST_TEST(!JsonObject::parse(deep));
    BOOST_TEST(!parseReadsbLine(deep));
    std::string in_object = R"({"now":1,"hex":"abcdef","x":)" + std::string(30000, '[') + "1" +
                            std::string(30000, ']') + "}";
    BOOST_TEST(!parseReadsbLine(in_object));
    const bool fast = std::chrono::steady_clock::now() - start < std::chrono::seconds(1);
    BOOST_TEST(fast);

    // At the limit it still parses; brackets inside strings do not count.
    std::string at_limit = "{\"x\":" + std::string(JsonObject::kMaxJsonDepth - 1, '[') +
                           std::string(JsonObject::kMaxJsonDepth - 1, ']') + "}";
    BOOST_TEST(JsonObject::parse(at_limit).has_value());
    std::string over = "{\"x\":" + std::string(JsonObject::kMaxJsonDepth, '[') +
                       std::string(JsonObject::kMaxJsonDepth, ']') + "}";
    BOOST_TEST(!JsonObject::parse(over));
    auto quoted = JsonObject::parse(R"({"s":"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[\"{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{"})");
    BOOST_REQUIRE(quoted);
    BOOST_TEST(quoted->getString("s").value_or("").size() == 81u);
}

BOOST_AUTO_TEST_CASE(deep_object_nesting_rejected)
{
    auto start = std::chrono::steady_clock::now();
    std::string deep;
    for (int i = 0; i < 30000; ++i)
        deep += "{\"a\":";
    BOOST_TEST(!JsonObject::parse(deep));
    BOOST_TEST(!parseReadsbLine(deep));
    deep += "1";
    deep += std::string(30000, '}');
    BOOST_TEST(!parseReadsbLine(deep));

    // 30,000 levels is over the size cap anyway; 12,000 (60,000 bytes) is not,
    // so this one reaches the depth check.
    std::string under_cap;
    for (int i = 0; i < 12000; ++i)
        under_cap += "{\"a\":";
    BOOST_REQUIRE(under_cap.size() < JsonObject::kMaxTextBytes);
    BOOST_TEST(!JsonObject::parse(under_cap));
    BOOST_TEST(!parseReadsbLine(under_cap));
    const bool fast = std::chrono::steady_clock::now() - start < std::chrono::seconds(1);
    BOOST_TEST(fast);
}

BOOST_AUTO_TEST_CASE(invalid_utf8_rejected_or_ignored)
{
    const std::string line = std::string(R"({"now":5,"hex":"abcdef","type":"adsb_icao","flight":"AB)") +
                             "\xff\xfe" + R"(","lat":38.9,"lon":-77})";
    std::optional<skycued::TrackObservation> obs;
    BOOST_CHECK_NO_THROW(obs = parseReadsbLine(line));
    if (obs) {
        BOOST_TEST(obs->id.address == 0xabcdefu);
    }
    BOOST_CHECK_NO_THROW(JsonObject::parse("{\"\xff\xfe\":1}"));
}

// Fuzz finding (fuzz_readsb_line, minimised): "now" of 1.79e10 s passed the
// old 1e11 bound, but system_clock counts int64 nanoseconds, which end near
// 9.22e9 s; the double-to-int64 conversion overflowed (undefined behaviour,
// a garbage received_time).
BOOST_AUTO_TEST_CASE(fuzz_readsb_line_now_overflow)
{
    const std::string line =
        fromHex("7b226e6f77223a31373933353435303235362c22686578223a22613466326331227d");
    BOOST_TEST(line == R"({"now":17935450256,"hex":"a4f2c1"})");
    BOOST_TEST(!parseReadsbLine(line).has_value());

    // The largest accepted "now" still converts exactly enough.
    auto obs = parseReadsbLine(R"({"now":9000000000,"hex":"a4f2c1","lat":1,"lon":2})");
    BOOST_TEST_REQUIRE(obs.has_value());
    BOOST_TEST(std::fabs(toSeconds(obs->received_time) - 9e9) < 1e-3);
    BOOST_TEST(std::fabs(toSeconds(*obs->position_time) - 9e9) < 1e-3);
    BOOST_TEST(!parseReadsbLine(R"({"now":9000000001,"hex":"a4f2c1"})").has_value());
}

BOOST_AUTO_TEST_CASE(sample_parses_fully)
{
    const std::string path = SKYCUE_TEST_DATA_DIR "/readsb-sample.jsonl";
    std::ifstream in(path, std::ios::binary);
    BOOST_REQUIRE_MESSAGE(in, "cannot open " << path);
    std::string line;
    int lines = 0, parsed = 0, on_ground = 0, with_position = 0, uat = 0, tisb = 0, non_icao = 0;
    while (std::getline(in, line)) {
        ++lines;
        auto obs = parseReadsbLine(line);
        BOOST_TEST_CONTEXT("line " << lines)
        {
            BOOST_TEST(obs.has_value());
        }
        if (!obs)
            continue;
        ++parsed;
        on_ground += obs->on_ground.value_or(false) ? 1 : 0;
        with_position += obs->lat ? 1 : 0;
        uat += obs->source == Source::uat_978 ? 1 : 0;
        tisb += obs->source == Source::tisb ? 1 : 0;
        non_icao += obs->id.type == AddressType::non_icao ? 1 : 0;
    }
    // gen_readsb_jsonl.py: 11 aircraft x 121 lines (t = 0..120 s at 1 Hz), one that
    // stops at t = 60 s (60 lines), then 3 odd lines.
    BOOST_TEST(lines == 11 * 121 + 60 + 3);
    BOOST_TEST(parsed == lines);
    BOOST_TEST(on_ground >= 121);
    BOOST_TEST(with_position == lines - 30 - 1);  // 30 s without position, 1 odd line
    BOOST_TEST(uat == 2 * 121);
    BOOST_TEST(tisb == 121);
    BOOST_TEST(non_icao == 121);
}

BOOST_AUTO_TEST_CASE(json_writer_escapes)
{
    JsonWriter w;
    w.add("s", std::string("a\"b\n"))
        .add("bs", std::string("c\\d\t\x01"))
        .add("d", 38.912345678)
        .add("small", 0.5)
        .add("nan", std::nan(""))
        .add("i", static_cast<int64_t>(-1234567890123))
        .add("t", true)
        .add("f", false)
        .add("k\"ey", std::string(""));
    BOOST_TEST(w.str() ==
               R"({"s":"a\"b\n","bs":"c\\d\t\u0001","d":38.91235,"small":0.5,"nan":null,)"
               R"("i":-1234567890123,"t":true,"f":false,"k\"ey":""})");
    BOOST_TEST(JsonWriter().str() == "{}");

    // Round trip through the parser.
    auto o = JsonObject::parse(w.str());
    BOOST_REQUIRE(o);
    BOOST_TEST(o->getString("s").value_or("") == "a\"b\n");
    BOOST_TEST(o->getString("bs").value_or("") == "c\\d\t\x01");
    BOOST_TEST(o->getInt("i").value_or(0) == -1234567890123);
}

BOOST_AUTO_TEST_CASE(json_object_array)
{
    auto o = JsonObject::parse(
        R"({"a":[{"x":1},{"x":"2"}],"empty":[],"eobj":{},"scalars":[1,2],"nested":[[1]],)"
        R"("obj":{"k":{"x":1}},"s":"v","mixed":[{"x":1},3]})");
    BOOST_REQUIRE(o);
    auto a = o->getObjectArray("a");
    BOOST_REQUIRE(a);
    BOOST_REQUIRE(a->size() == 2u);
    BOOST_TEST((*a)[0].getInt("x").value_or(0) == 1);
    BOOST_TEST((*a)[1].getInt("x").value_or(0) == 2);
    BOOST_TEST(o->getObjectArray("empty").value().empty());
    BOOST_TEST(o->getObjectArray("eobj").value().empty());  // {} reads as an empty array
    BOOST_TEST(!o->getObjectArray("scalars"));
    BOOST_TEST(!o->getObjectArray("nested"));
    BOOST_TEST(!o->getObjectArray("obj"));
    BOOST_TEST(!o->getObjectArray("s"));
    BOOST_TEST(!o->getObjectArray("mixed"));
    BOOST_TEST(!o->getObjectArray("absent"));
}

BOOST_AUTO_TEST_CASE(json_take_object)
{
    auto o = JsonObject::parse(R"({"obj":{"k":{"x":1},"a":[1,2]},"e":{},"arr":[1],"s":"v"})");
    BOOST_REQUIRE(o);
    auto obj = o->takeObject("obj");
    BOOST_REQUIRE(obj);
    BOOST_TEST(obj->getObject("k").value().getInt("x").value_or(0) == 1);
    BOOST_TEST(obj->getStringArray("a").value().size() == 2u);
    // Moved out: the key stays, holding an empty object.
    BOOST_TEST(o->has("obj"));
    BOOST_TEST(o->getObject("obj").value().keys().empty());
    BOOST_TEST(o->takeObject("e").value().keys().empty());
    BOOST_TEST(!o->takeObject("arr"));
    BOOST_TEST(o->getStringArray("arr").value().size() == 1u);  // untouched
    BOOST_TEST(!o->takeObject("s"));
    BOOST_TEST(!o->takeObject("absent"));
    // A move swaps the tree: the moved-to object has it.
    JsonObject moved(std::move(*obj));
    BOOST_TEST(moved.has("k"));
}

BOOST_AUTO_TEST_CASE(json_bool)
{
    auto o = JsonObject::parse(R"({"t":true,"f":false,"one":1,"s":"yes","o":{},"st":"true"})");
    BOOST_REQUIRE(o);
    BOOST_TEST((o->getBool("t") == std::optional<bool>(true)));
    BOOST_TEST((o->getBool("f") == std::optional<bool>(false)));
    BOOST_TEST(!o->getBool("one"));
    BOOST_TEST(!o->getBool("s"));
    BOOST_TEST(!o->getBool("o"));
    BOOST_TEST(!o->getBool("absent"));
    // PropertyTree keeps scalars as text, so the string "true" reads the same.
    BOOST_TEST((o->getBool("st") == std::optional<bool>(true)));
}

// Every JSON text the daemon writes is valid UTF-8: a byte that does not
// start a well-formed RFC 3629 sequence, or a sequence cut short, becomes
// U+FFFD, one per byte; valid sequences pass unchanged.
BOOST_AUTO_TEST_CASE(json_writer_output_is_valid_utf8)
{
    const auto w = [](const std::string& s) { return skycued::JsonWriter().add("k", s).str(); };
    BOOST_TEST(w("a\xFF" "b") == "{\"k\":\"a\xEF\xBF\xBD" "b\"}");
    BOOST_TEST(w("\xC0\xAF") == "{\"k\":\"\xEF\xBF\xBD\xEF\xBF\xBD\"}");                  // overlong
    BOOST_TEST(w("\xED\xA0\x80") == "{\"k\":\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\"}");  // surrogate
    BOOST_TEST(w("\xC3\xA9t\xC3\xA9") == "{\"k\":\"\xC3\xA9t\xC3\xA9\"}");                // valid kept
    BOOST_TEST(w("\xE2\x82") == "{\"k\":\"\xEF\xBF\xBD\xEF\xBF\xBD\"}");                  // truncated
    BOOST_TEST(w("\xF4\x90\x80\x80") == "{\"k\":\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\"}");  // > U+10FFFF
    BOOST_TEST(w("\xF0\x9F\x9B\xA9\x7F") == "{\"k\":\"\xF0\x9F\x9B\xA9\x7F\"}");          // 4 bytes, DEL kept
}

BOOST_AUTO_TEST_CASE(readsb_tisb_type_sets_rebroadcast)
{
    auto tisb = parseReadsbLine(R"({"now":5,"hex":"abc123","type":"tisb_icao","lat":38.95,"lon":-77.1})");
    BOOST_REQUIRE(tisb);
    BOOST_TEST(tisb->rebroadcast);
    auto adsr = parseReadsbLine(R"({"now":5,"hex":"abc123","type":"adsr_icao","lat":38.95,"lon":-77.1})");
    BOOST_REQUIRE(adsr);
    BOOST_TEST(!adsr->rebroadcast);
    auto direct = parseReadsbLine(R"({"now":5,"hex":"abc123","type":"adsb_icao","lat":38.95,"lon":-77.1})");
    BOOST_REQUIRE(direct);
    BOOST_TEST(!direct->rebroadcast);
}

BOOST_AUTO_TEST_CASE(json_string_array_escapes_and_has_no_spaces)
{
    using namespace skycued;
    BOOST_CHECK_EQUAL(jsonStringArray({}), "[]");
    BOOST_CHECK_EQUAL(jsonStringArray({"a", "b\"c", "d\\e", "\n"}), R"(["a","b\"c","d\\e","\n"])");
    BOOST_CHECK_EQUAL(jsonString("x\ty"), R"("x\ty")");
}

BOOST_AUTO_TEST_CASE(json_writer_add_opt_and_finite)
{
    using namespace skycued;
    BOOST_CHECK_EQUAL(JsonWriter().addOpt("a", std::optional<int32_t>(5)).addOpt("b", std::optional<int32_t>()).str(),
                      R"({"a":5})");
    BOOST_CHECK_EQUAL(JsonWriter().addOpt("f", std::optional<float>(1.5f)).str(), R"({"f":1.5})");
    BOOST_CHECK_EQUAL(JsonWriter().addOpt("f", std::optional<float>(NAN)).str(), "{}");
    BOOST_CHECK_EQUAL(JsonWriter().addOpt("f", std::optional<float>(INFINITY)).str(), "{}");
    BOOST_CHECK_EQUAL(JsonWriter().addOpt("s", std::optional<std::string>("x")).str(), R"({"s":"x"})");
    BOOST_CHECK_EQUAL(JsonWriter().addOpt("s", std::optional<std::string>()).str(), "{}");
    BOOST_CHECK_EQUAL(JsonWriter().addFinite("d", NAN).str(), "{}");
    BOOST_CHECK_EQUAL(JsonWriter().addFinite("d", 2.0).str(), R"({"d":2})");
    BOOST_CHECK_EQUAL(JsonWriter().addOptionalText("t", "").str(), R"({"t":null})");
    BOOST_CHECK_EQUAL(JsonWriter().addOptionalText("t", "a b").str(), R"({"t":"a b"})");
}

namespace {
struct JsonGetError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
}  // namespace

BOOST_AUTO_TEST_CASE(json_get_helpers_throw_the_given_type_with_the_exact_text)
{
    namespace jg = skycued::json_get;
    const auto o = JsonObject::parse(R"({"n":5,"s":"x","b":true,"z":null})");
    BOOST_REQUIRE(o);
    BOOST_CHECK_EQUAL(jg::text<JsonGetError>(*o, "s", "a string"), "x");
    BOOST_CHECK_EXCEPTION(jg::text<JsonGetError>(*o, "q", "a string"), JsonGetError,
                          [](const JsonGetError& e) { return std::string(e.what()) == "q must be a string"; });
    BOOST_CHECK_EQUAL(jg::intIn<JsonGetError>(*o, "n", 1, 9), 5);
    BOOST_CHECK_EXCEPTION(jg::intIn<JsonGetError>(*o, "n", 6, 9), JsonGetError, [](const JsonGetError& e) {
        return std::string(e.what()) == "n must be an integer from 6 to 9";
    });
    BOOST_CHECK(jg::flag<JsonGetError>(*o, "b"));
    BOOST_CHECK_EXCEPTION(jg::flag<JsonGetError>(*o, "s"), JsonGetError,
                          [](const JsonGetError& e) { return std::string(e.what()) == "s must be true or false"; });
    BOOST_CHECK_EQUAL(jg::optionalText<JsonGetError>(*o, "q", "a string"), "");
    BOOST_CHECK_EQUAL(jg::optionalText<JsonGetError>(*o, "z", "a string"), "");
    BOOST_CHECK_EQUAL(jg::optionalText<JsonGetError>(*o, "s", "a string"), "x");
}
