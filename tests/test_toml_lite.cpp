#define BOOST_TEST_MODULE skycued_toml_lite
#include <boost/test/unit_test.hpp>

#include "toml_lite.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

using namespace skycued::toml_lite;

namespace {

const Value& at(const Document& d, const std::string& key)
{
    auto it = d.entries.find(key);
    BOOST_REQUIRE_MESSAGE(it != d.entries.end(), "missing key " << key);
    return it->second.value;
}

std::string str(const Document& d, const std::string& k) { return std::get<std::string>(at(d, k)); }
int64_t num(const Document& d, const std::string& k) { return std::get<int64_t>(at(d, k)); }
double flt(const Document& d, const std::string& k) { return std::get<double>(at(d, k)); }

// The error's line and message must name `line` and contain `needle`.
void expectError(const std::string& text, int line, const std::string& needle)
{
    try {
        parse(text, "t.toml");
    } catch (const Error& e) {
        BOOST_CHECK_EQUAL(e.line, line);
        const std::string what = e.what();
        BOOST_CHECK_MESSAGE(what.rfind("t.toml:" + std::to_string(line) + ": ", 0) == 0, what);
        BOOST_CHECK_MESSAGE(what.find(needle) != std::string::npos, what << " lacks '" << needle << "'");
        return;
    }
    BOOST_ERROR("no error for: " << text);
}

}  // namespace

BOOST_AUTO_TEST_CASE(empty_document)
{
    for (const char* t : {"", "\n", "  \n# only a comment\n\n"}) {
        const Document d = parse(t, "t");
        BOOST_CHECK(d.entries.empty());
        BOOST_CHECK(d.tables.empty());
    }
}

BOOST_AUTO_TEST_CASE(tables_and_keys_dotted)
{
    const Document d = parse("top = 1\n[a]\nx = 2\n[b]\n[c.d]\ny_1 = \"v\"\n", "t");
    BOOST_CHECK_EQUAL(num(d, "top"), 1);
    BOOST_CHECK_EQUAL(num(d, "a.x"), 2);
    BOOST_CHECK_EQUAL(str(d, "c.d.y_1"), "v");
    BOOST_CHECK_EQUAL(d.entries.size(), 3u);
    BOOST_CHECK_EQUAL(d.entries.at("a.x").line, 3);
    BOOST_CHECK_EQUAL(d.entries.at("c.d.y_1").line, 6);
    BOOST_CHECK(d.tables.count("a") && d.tables.count("b") && d.tables.count("c.d"));
    BOOST_CHECK_EQUAL(d.tables.size(), 3u);
}

BOOST_AUTO_TEST_CASE(nested_table_header)
{
    const Document d = parse("[inputs]\nf = 1\n[inputs.readsb]\nenable = true\n[ inputs . dump978 ]\nport = 5\n", "t");
    BOOST_CHECK(d.tables.count("inputs") && d.tables.count("inputs.readsb") && d.tables.count("inputs.dump978"));
    BOOST_CHECK_EQUAL(num(d, "inputs.f"), 1);
    BOOST_CHECK(std::get<bool>(at(d, "inputs.readsb.enable")));
    BOOST_CHECK_EQUAL(num(d, "inputs.dump978.port"), 5);
    // A parent opened after its child is fine.
    const Document e = parse("[a.b]\nx=1\n[a]\ny=2\n", "t");
    BOOST_CHECK_EQUAL(num(e, "a.y"), 2);
}

BOOST_AUTO_TEST_CASE(basic_string_escapes)
{
    const Document d = parse("a = \"q\\\" b\\\\ n\\n t\\t r\\r\"\nb = \"\\u00e9\\u20AC\"\nc = \"\"\nd = \"it's # not a comment\"\n", "t");
    BOOST_CHECK_EQUAL(str(d, "a"), "q\" b\\ n\n t\t r\r");
    BOOST_CHECK_EQUAL(str(d, "b"), "\xC3\xA9\xE2\x82\xAC");
    BOOST_CHECK_EQUAL(str(d, "c"), "");
    BOOST_CHECK_EQUAL(str(d, "d"), "it's # not a comment");
    expectError("a = \"\\x\"\n", 1, "escape");
    expectError("a = \"\\u12\"\n", 1, "four");
    expectError("a = \"\\uD800\"\n", 1, "surrogate");
}

BOOST_AUTO_TEST_CASE(literal_string)
{
    const Document d = parse("a = 'C:\\path\\n \"quoted\"'\nb = ''\n", "t");
    BOOST_CHECK_EQUAL(str(d, "a"), "C:\\path\\n \"quoted\"");
    BOOST_CHECK_EQUAL(str(d, "b"), "");
}

BOOST_AUTO_TEST_CASE(integer_with_sign_and_underscores)
{
    const Document d = parse("a = 0\nb = +7\nc = -42\nd = 1_000_000\ne = -9223372036854775808\nf = 9223372036854775807\n", "t");
    BOOST_CHECK_EQUAL(num(d, "a"), 0);
    BOOST_CHECK_EQUAL(num(d, "b"), 7);
    BOOST_CHECK_EQUAL(num(d, "c"), -42);
    BOOST_CHECK_EQUAL(num(d, "d"), 1000000);
    BOOST_CHECK_EQUAL(num(d, "e"), INT64_MIN);
    BOOST_CHECK_EQUAL(num(d, "f"), INT64_MAX);
    expectError("a = 9223372036854775808\n", 1, "range");
    expectError("a = 1__0\n", 1, "invalid");
    expectError("a = _1\n", 1, "invalid");
    expectError("a = 1_\n", 1, "invalid");
    expectError("a = 007\n", 1, "leading zero");
    expectError("a = 0x10\n", 1, "invalid");
    expectError("a = hello\n", 1, "invalid");
}

BOOST_AUTO_TEST_CASE(float_and_exponent)
{
    const Document d = parse("a = 0.0\nb = -1.5\nc = 1e3\nd = 2.5E-2\ne = +3.25e+1\nf = 1_0.2_5\n", "t");
    BOOST_CHECK_EQUAL(flt(d, "a"), 0.0);
    BOOST_CHECK_EQUAL(flt(d, "b"), -1.5);
    BOOST_CHECK_EQUAL(flt(d, "c"), 1000.0);
    BOOST_CHECK_EQUAL(flt(d, "d"), 0.025);
    BOOST_CHECK_EQUAL(flt(d, "e"), 32.5);
    BOOST_CHECK_EQUAL(flt(d, "f"), 10.25);
    expectError("a = 1.\n", 1, "invalid");
    expectError("a = .5\n", 1, "invalid");
    expectError("a = 1e\n", 1, "invalid");
    expectError("a = 1e999\n", 1, "range");
    expectError("a = inf\n", 1, "invalid");
    expectError("a = nan\n", 1, "invalid");
}

BOOST_AUTO_TEST_CASE(booleans)
{
    const Document d = parse("a = true\nb = false # c\n", "t");
    BOOST_CHECK(std::get<bool>(at(d, "a")));
    BOOST_CHECK(!std::get<bool>(at(d, "b")));
    expectError("a = True\n", 1, "invalid");
}

BOOST_AUTO_TEST_CASE(string_array_multiline_trailing_comma)
{
    const Document d = parse(
        "a = []\nb = [\"x\", 'y' ,\"z\"]\nc = [\n  \"one\", # first\n  \"two\",\n\n]\nd = [ ]\ne = 1\n", "t");
    BOOST_CHECK(std::get<Array>(at(d, "a")).empty());
    BOOST_CHECK((std::get<Array>(at(d, "b")) == Array{"x", "y", "z"}));
    BOOST_CHECK((std::get<Array>(at(d, "c")) == Array{"one", "two"}));
    BOOST_CHECK(std::get<Array>(at(d, "d")).empty());
    BOOST_CHECK_EQUAL(d.entries.at("c").line, 3);
    BOOST_CHECK_EQUAL(d.entries.at("e").line, 9);  // line counting survives a multi-line array
    expectError("a = [\"x\"\n", 2, "unterminated");
    expectError("a = [\"x\" \"y\"]\n", 1, "','");
    expectError("a = [,]\n", 1, "strings");
}

BOOST_AUTO_TEST_CASE(comments_and_crlf)
{
    const Document d = parse("# c\r\n[a] # t\r\nx = 1 # c\r\ny = [\r\n \"p\",\r\n \"q\"\r\n]\r\n\r\nz = 'w'", "t");
    BOOST_CHECK_EQUAL(num(d, "a.x"), 1);
    BOOST_CHECK((std::get<Array>(at(d, "a.y")) == Array{"p", "q"}));
    BOOST_CHECK_EQUAL(str(d, "a.z"), "w");
    BOOST_CHECK_EQUAL(d.entries.at("a.z").line, 9);
    expectError("a = 1\rb = 2\n", 1, "carriage");
}

BOOST_AUTO_TEST_CASE(error_duplicate_key_names_line)
{
    expectError("[a]\nx = 1\ny = 2\nx = 3\n", 4, "a.x");
    expectError("x = 1\nx = 2\n", 2, "twice");
    try {
        parse("[a]\nx = 1\nx = 2\n", "cfg.toml");
        BOOST_ERROR("no error");
    } catch (const Error& e) {
        BOOST_CHECK(std::string(e.what()).rfind("cfg.toml:3: ", 0) == 0);
    }
}

BOOST_AUTO_TEST_CASE(error_duplicate_table)
{
    expectError("[a]\nx = 1\n[b]\n[a]\n", 4, "twice");
    expectError("[a]\n[a]\n", 2, "twice");
    expectError("[a.b]\n[c]\n[a.b]\n", 3, "twice");
    expectError("[a]\nb = 1\n[a.b]\n", 3, "also a key");
    expectError("[a.b]\n[a]\nb = 1\n", 3, "also a table");
}

BOOST_AUTO_TEST_CASE(error_table_below_a_key)
{
    expectError("x = 1\n[x.y]\n", 2, "is a key");
    expectError("[a]\nb = 1\n[a.b.c]\n", 3, "a.b is a key");
    expectError("[a]\nb = 1\n[a.b.c.d]\n", 3, "a.b is a key");
    expectError("[a.b.c]\n[a]\nb = 1\n", 3, "also a table");
    BOOST_CHECK_NO_THROW(parse("x = 1\n[y.z]\n[y]\n", "t.toml"));  // another name is fine
}

BOOST_AUTO_TEST_CASE(error_dotted_key_in_assignment)
{
    expectError("a.b = 1\n", 1, "dotted");
    expectError("[t]\nx = 1\na . b = 1\n", 3, "dotted");
    expectError("\"a\" = 1\n", 1, "quoted");
    expectError("[a..b]\n", 1, "table name");
    expectError("[a.]\n", 1, "table name");
    expectError("[[a]]\n", 1, "arrays of tables");
}

BOOST_AUTO_TEST_CASE(error_inline_table)
{
    expectError("a = { b = 1 }\n", 1, "inline");
    expectError("a = [{ b = 1 }]\n", 1, "inline");
}

BOOST_AUTO_TEST_CASE(error_multiline_string)
{
    expectError("a = \"\"\"x\"\"\"\n", 1, "multi-line");
    expectError("\n a = '''x'''\n", 2, "multi-line");
    expectError("a = [\"\"\"x\"\"\"]\n", 1, "multi-line");
}

BOOST_AUTO_TEST_CASE(error_datetime)
{
    expectError("a = 1979-05-27\n", 1, "date");
    expectError("\na = 1979-05-27T07:32:00Z\n", 2, "date");
    expectError("a = 07:32:00\n", 1, "date");
}

BOOST_AUTO_TEST_CASE(error_array_of_ints)
{
    expectError("a = [1, 2]\n", 1, "strings only");
    expectError("a = [\"x\", 2]\n", 1, "strings only");
    expectError("a = [true]\n", 1, "strings only");
    expectError("a = [[\"x\"]]\n", 1, "nested");
}

BOOST_AUTO_TEST_CASE(error_bare_key_chars)
{
    expectError("a-b = 1\n", 1, "key");
    expectError("a b = 1\n", 1, "key");
    expectError("[a-b]\n", 1, "table");
    expectError("é = 1\n", 1, "key");
    expectError("= 1\n", 1, "key");
    expectError("a =\n", 1, "missing value");
    expectError("a\n", 1, "'='");
    expectError("a = 1 b = 2\n", 1, "after the value");
    expectError("[a\n", 1, "unterminated");
    expectError("[a] x\n", 1, "after the value");
}

BOOST_AUTO_TEST_CASE(error_unterminated_string)
{
    expectError("a = \"abc\n", 1, "unterminated");
    expectError("\n\na = 'abc", 3, "unterminated");
    expectError("a = \"abc\\", 1, "unterminated");
    expectError("a = \"ab\x01\"\n", 1, "control");
}

BOOST_AUTO_TEST_CASE(format_value_roundtrip)
{
    const Value values[] = {
        Value(std::in_place_type<std::string>, "plain"),
        Value(std::in_place_type<std::string>, std::string("q\" b\\ n\n t\t r\r e\x01 \x7f é # x", 24)),
        Value(std::in_place_type<std::string>, ""),
        Value(std::in_place_type<int64_t>, 0),
        Value(std::in_place_type<int64_t>, INT64_MIN),
        Value(std::in_place_type<int64_t>, INT64_MAX),
        Value(std::in_place_type<double>, 0.0),
        Value(std::in_place_type<double>, 1.0),
        Value(std::in_place_type<double>, -37.887976),
        Value(std::in_place_type<double>, 1e20),
        Value(std::in_place_type<double>, 0.1),
        Value(std::in_place_type<bool>, true),
        Value(std::in_place_type<bool>, false),
        Value(std::in_place_type<Array>, Array{}),
        Value(std::in_place_type<Array>, Array{"a", "b\"c", ""}),
    };
    for (const Value& v : values) {
        const std::string text = formatValue(v);
        const Document d = parse("k = " + text + "\n", "t");
        BOOST_CHECK_MESSAGE(at(d, "k") == v, "round trip of " << text);
    }
    BOOST_CHECK_EQUAL(formatValue(Value(std::in_place_type<double>, 1.0)), "1.0");
    BOOST_CHECK_EQUAL(formatValue(Value(std::in_place_type<bool>, true)), "true");
    BOOST_CHECK_EQUAL(formatValue(Value(std::in_place_type<Array>, Array{"a", "b"})), "[\"a\", \"b\"]");
    BOOST_CHECK_THROW(formatValue(Value(std::in_place_type<double>, INFINITY)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(unicode_escape_as_the_page_writes_it)
{
    // The Config tab's copy writes control characters as \uXXXX and leaves other
    // characters as they are; both read back.
    const Document d = parse("k = \"caf\\u00E9 \\u0001\\u007F \\u20AC\"\nj = \"caf\xC3\xA9\"\n", "t");
    BOOST_CHECK_EQUAL(str(d, "k"), "caf\xC3\xA9 \x01\x7F \xE2\x82\xAC");
    BOOST_CHECK_EQUAL(str(d, "j"), "caf\xC3\xA9");
    const std::string text = formatValue(Value(std::string("caf\xC3\xA9 \x01 \xE2\x82\xAC")));
    BOOST_CHECK_EQUAL(str(parse("k = " + text + "\n", "t"), "k"), "caf\xC3\xA9 \x01 \xE2\x82\xAC");
}

BOOST_AUTO_TEST_CASE(string_as_tomlString_in_app_pure_js_emits_it)
{
    // www/app_pure.js tomlValue('recording.dir', 'caf\u00e9 "q" back\\slash \u20ac') (tomlString), copied exactly.
    const std::string emitted = "\"caf\xC3\xA9 \\\"q\\\" back\\\\slash \xE2\x82\xAC\"";
    const Document d = parse("k = " + emitted + "\n", "t");
    BOOST_CHECK_EQUAL(str(d, "k"), "caf\xC3\xA9 \"q\" back\\slash \xE2\x82\xAC");
}

BOOST_AUTO_TEST_CASE(design_example_file_parses)
{
    std::ifstream in(std::string(SKYCUE_FIXTURE_DIR) + "/skycue_example.toml", std::ios::binary);
    BOOST_REQUIRE(in);
    std::stringstream ss;
    ss << in.rdbuf();
    const Document d = parse(ss.str(), "skycue_example.toml");
    BOOST_CHECK_EQUAL(flt(d, "receiver.lat"), 0.0);
    BOOST_CHECK_EQUAL(str(d, "inputs.decoder"), "readsb");
    BOOST_CHECK_EQUAL(num(d, "inputs.readsb.json_port"), 30047);
    BOOST_CHECK_EQUAL(num(d, "filters.max_range_nm"), 150);
    BOOST_CHECK((std::get<Array>(at(d, "recording.formats")) == Array{"jsonl", "csv"}));
    BOOST_CHECK(std::get<Array>(at(d, "cues.tcp_connect")).empty());
    BOOST_CHECK_EQUAL(num(d, "stats.save_s"), 300);
    BOOST_CHECK_EQUAL(d.tables.size(), 12u);
}
