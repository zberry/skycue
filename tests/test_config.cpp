#define BOOST_TEST_MODULE skycued_config
#include <boost/test/unit_test.hpp>

#include "config.hpp"
#include "log.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

using skycued::Config;
using skycued::ConfigError;
using skycued::parseConfigText;

namespace {

const char* const kLatLon = "RECEIVER_LAT=51.5\nRECEIVER_LON=-0.12\n";

std::string readExample()
{
    std::ifstream in(SKYCUE_EXAMPLE_CONF, std::ios::binary);
    BOOST_REQUIRE_MESSAGE(in, "cannot open " SKYCUE_EXAMPLE_CONF);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void replaceOnce(std::string& s, const std::string& from, const std::string& to)
{
    auto pos = s.find(from);
    BOOST_REQUIRE(pos != std::string::npos);
    s.replace(pos, from.size(), to);
}

// Parses text expected to be rejected; returns the error.
ConfigError expectError(const std::string& text)
{
    try {
        parseConfigText(text, "test.conf");
    } catch (const ConfigError& e) {
        return e;
    }
    BOOST_FAIL("expected ConfigError for: " + text);
    throw;  // unreachable
}

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

}  // namespace

BOOST_AUTO_TEST_CASE(parses_example_file)
{
    std::string text = readExample();
    replaceOnce(text, "\nRECEIVER_LAT=\n", "\nRECEIVER_LAT=51.5\n");
    replaceOnce(text, "\nRECEIVER_LON=\n", "\nRECEIVER_LON=-0.12\n");
    Config c = parseConfigText(text, "skycue.conf.example");

    BOOST_TEST(c.receiver_lat == 51.5);
    BOOST_TEST(c.receiver_lon == -0.12);
    BOOST_TEST(c.receiver_alt_m == 0.0);
    BOOST_TEST(c.antenna_height_m == 0.0);
    BOOST_TEST(c.dump978_raw_port == 30978);
    BOOST_TEST(c.dump978_json_port == 30979);
    BOOST_TEST(c.readsb_json_port == 30047);
    BOOST_TEST(c.web_port == 8080);
    BOOST_TEST(c.cue_tcp_port == 30500);
    BOOST_TEST(c.cue_udp_targets.empty());
    BOOST_TEST(c.record_dir.empty());
    BOOST_TEST((c.record_formats == std::set<std::string>{"jsonl", "csv"}));
    BOOST_TEST(c.max_range_nm == 150.0);
    BOOST_TEST(c.min_elevation_deg == 0.0);
    BOOST_TEST(c.allow_list_file.empty());
    BOOST_TEST(c.block_list_file.empty());
    BOOST_TEST(c.min_interval_s == 1.0);
    BOOST_TEST(c.max_interval_s == 10.0);
    BOOST_TEST(c.track_timeout_s == 60.0);
    BOOST_TEST(c.keyframe_interval_s == 10.0);
    BOOST_TEST(c.sdr_1090_serial.empty());
    BOOST_TEST(c.sdr_978_serial.empty());
    BOOST_TEST(c.gain_1090 == "auto");
    BOOST_TEST(c.gain_978 == "auto");
}

BOOST_AUTO_TEST_CASE(parses_example_file_from_disk_needs_lat_lon)
{
    // The shipped example leaves the required keys empty.
    try {
        skycued::parseConfigFile(SKYCUE_EXAMPLE_CONF);
        BOOST_FAIL("expected ConfigError");
    } catch (const ConfigError& e) {
        BOOST_TEST(contains(e.what(), "missing required key RECEIVER_LAT"));
    }
}

BOOST_AUTO_TEST_CASE(unknown_key_rejected)
{
    ConfigError e = expectError(std::string("FOO=1\n") + kLatLon);
    BOOST_TEST(e.line == 1);
    BOOST_TEST(contains(e.what(), "FOO"));
    BOOST_TEST(contains(e.what(), "test.conf:1:"));
}

BOOST_AUTO_TEST_CASE(shell_expansion_rejected)
{
    for (const char* line : {"RECEIVER_LAT=$(echo 1)", "RECEIVER_LAT=`echo 1`",
                             "RECEIVER_LAT=\"$HOME\"", "RECEIVER_LAT=\"a\\b\""}) {
        ConfigError e = expectError(std::string(line) + "\nRECEIVER_LON=1\n");
        BOOST_TEST(e.line == 1);
        BOOST_TEST(contains(e.what(), "malformed"), line);
    }
}

BOOST_AUTO_TEST_CASE(malformed_lines_rejected)
{
    // Same accept/reject decisions as load_config in skycue-start.sh.
    for (const char* line : {" RECEIVER_ALT_M=1", "RECEIVER_ALT_M = 1", "RECEIVER_ALT_M =1",
                             "receiver_alt_m=1", "_X=1", "RECEIVER_ALT_M", "1A=2",
                             "RECEIVER_ALT_M=1 # trailing comment", "RECEIVER_ALT_M=1 ",
                             "RECEIVER_ALT_M=\"1\" ", "RECEIVER_ALT_M=\"1", "RECEIVER_ALT_M='1'x",
                             "RECEIVER_ALT_M='it's'", "RECEIVER_ALT_M=a b"}) {
        ConfigError e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(e.line == 3, line);
        BOOST_TEST(contains(e.what(), "malformed"), line);
    }
}

BOOST_AUTO_TEST_CASE(quoted_values)
{
    Config c = parseConfigText(std::string(kLatLon) + "RECORD_FORMATS=\"jsonl csv\"\n", "t");
    BOOST_TEST((c.record_formats == std::set<std::string>{"jsonl", "csv"}));
    c = parseConfigText(std::string(kLatLon) + "RECORD_FORMATS='csv jsonl'\n", "t");
    BOOST_TEST((c.record_formats == std::set<std::string>{"jsonl", "csv"}));
    c = parseConfigText(std::string(kLatLon) + "RECORD_FORMATS=csv\n", "t");
    BOOST_TEST((c.record_formats == std::set<std::string>{"csv"}));

    c = parseConfigText(std::string(kLatLon) + "RECORD_DIR=\"/data/a # b\"\n", "t");
    BOOST_TEST(c.record_dir == "/data/a # b");
    c = parseConfigText(std::string(kLatLon) + "RECORD_DIR='/x $y `z` \\w #'\n", "t");
    BOOST_TEST(c.record_dir == "/x $y `z` \\w #");
    c = parseConfigText(std::string(kLatLon) + "RECORD_DIR=/a=b,c:d@e%f+g-h\n", "t");
    BOOST_TEST(c.record_dir == "/a=b,c:d@e%f+g-h");
    c = parseConfigText(std::string(kLatLon) + "RECORD_DIR=\"\"\n", "t");
    BOOST_TEST(c.record_dir.empty());

    ConfigError e = expectError(std::string(kLatLon) + "RECORD_FORMATS=\"jsonl xml\"\n");
    BOOST_TEST(e.line == 3);
    BOOST_TEST(contains(e.what(), "RECORD_FORMATS"));
}

BOOST_AUTO_TEST_CASE(comments_and_blank_lines)
{
    Config c = parseConfigText("# leading comment\n"
                               "\n"
                               "   \t\n"
                               "   # indented comment\n"
                               "RECEIVER_LAT=10\n"
                               "#RECEIVER_LON=99\n"
                               "RECEIVER_LON=20\n"
                               "MAX_RANGE_NM=5\n"
                               "MAX_RANGE_NM=7",  // last value wins; no final newline
                               "t");
    BOOST_TEST(c.receiver_lat == 10.0);
    BOOST_TEST(c.receiver_lon == 20.0);
    BOOST_TEST(c.max_range_nm == 7.0);
}

BOOST_AUTO_TEST_CASE(missing_required)
{
    ConfigError e = expectError("RECEIVER_LON=1\n");
    BOOST_TEST(contains(e.what(), "missing required key RECEIVER_LAT"));
    e = expectError("RECEIVER_LAT=1\n");
    BOOST_TEST(contains(e.what(), "missing required key RECEIVER_LON"));
    // An empty value counts as missing, as in skycue-start.sh.
    e = expectError("RECEIVER_LAT=\nRECEIVER_LON=1\n");
    BOOST_TEST(contains(e.what(), "missing required key RECEIVER_LAT"));
}

BOOST_AUTO_TEST_CASE(numeric_errors_name_key_and_line)
{
    ConfigError e = expectError(std::string(kLatLon) + "MAX_RANGE_NM=12abc\n");
    BOOST_TEST(e.line == 3);
    BOOST_TEST(contains(e.what(), "MAX_RANGE_NM"));
    for (const char* line : {"WEB_PORT=0", "WEB_PORT=65536", "WEB_PORT=-1", "WEB_PORT=+80",
                             "WEB_PORT=8o", "WEB_PORT=", "MIN_INTERVAL_S=nan",
                             "MIN_INTERVAL_S=inf", "MIN_INTERVAL_S="}) {
        e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(e.line == 3, line);
    }
    e = expectError("RECEIVER_LAT=91\nRECEIVER_LON=0\n");
    BOOST_TEST(e.line == 1);
    BOOST_TEST(contains(e.what(), "RECEIVER_LAT"));
    e = expectError("RECEIVER_LAT=0\nRECEIVER_LON=-180.5\n");
    BOOST_TEST(e.line == 2);

    Config c = parseConfigText(std::string(kLatLon) + "WEB_PORT=65535\nMIN_ELEVATION_DEG=-2.5\n", "t");
    BOOST_TEST(c.web_port == 65535);
    BOOST_TEST(c.min_elevation_deg == -2.5);
}

BOOST_AUTO_TEST_CASE(udp_targets)
{
    Config c = parseConfigText(std::string(kLatLon) +
                                   "CUE_UDP_TARGETS=\"10.0.0.5:4000 10.0.0.6:4001\"\n",
                               "t");
    BOOST_REQUIRE_EQUAL(c.cue_udp_targets.size(), 2u);
    BOOST_TEST(c.cue_udp_targets[0].first == "10.0.0.5");
    BOOST_TEST(c.cue_udp_targets[0].second == 4000);
    BOOST_TEST(c.cue_udp_targets[1].first == "10.0.0.6");
    BOOST_TEST(c.cue_udp_targets[1].second == 4001);

    c = parseConfigText(std::string(kLatLon) + "CUE_UDP_TARGETS=cuehost:30501\n", "t");
    BOOST_REQUIRE_EQUAL(c.cue_udp_targets.size(), 1u);
    BOOST_TEST(c.cue_udp_targets[0].first == "cuehost");

    for (const char* v : {"\"x\"", "x", ":4000", "h:", "h:0", "h:65536", "h:12a"}) {
        ConfigError e = expectError(std::string(kLatLon) + "CUE_UDP_TARGETS=" + v + "\n");
        BOOST_TEST(e.line == 3, v);
        BOOST_TEST(contains(e.what(), "CUE_UDP_TARGETS"), v);
    }
}

BOOST_AUTO_TEST_CASE(known_keys_match_example)
{
    std::string text = readExample();
    std::set<std::string> in_example;
    std::regex re("^([A-Z][A-Z0-9_]*)=");
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        std::smatch m;
        if (std::regex_search(line, m, re))
            in_example.insert(m[1].str());
    }
    const auto& known = skycued::knownConfigKeys();
    std::set<std::string> known_set(known.begin(), known.end());
    BOOST_TEST(known_set.size() == known.size());  // no duplicates
    for (const auto& k : in_example)
        BOOST_TEST(known_set.count(k) == 1u, "example key not known: " << k);
    for (const auto& k : known_set)
        BOOST_TEST(in_example.count(k) == 1u, "known key not in example: " << k);
    BOOST_TEST(in_example.size() > 20u);
}

BOOST_AUTO_TEST_CASE(log_line_format)
{
    using namespace std::chrono;
    // 2026-11-01T12:34:56.789Z
    system_clock::time_point t{milliseconds(1793536496789LL)};
    BOOST_TEST(skycued::formatLogLine(skycued::LogLevel::info, "hello 42", t) ==
               "2026-11-01T12:34:56.789Z INFO  hello 42\n");
    BOOST_TEST(skycued::formatLogLine(skycued::LogLevel::error, "x", t) ==
               "2026-11-01T12:34:56.789Z ERROR x\n");
    skycued::setLogLevel(skycued::LogLevel::warn);
    BOOST_TEST(!skycued::logEnabled(skycued::LogLevel::info));
    BOOST_TEST(skycued::logEnabled(skycued::LogLevel::error));
    int evaluated = 0;
    LOG_INFO("not built " << ++evaluated);
    BOOST_TEST(evaluated == 0);
    LOG_WARN("test_config log line " << ++evaluated);
    BOOST_TEST(evaluated == 1);
    skycued::setLogLevel(skycued::LogLevel::info);
}

BOOST_AUTO_TEST_CASE(crlf_line_rejected_with_line_number)
{
    ConfigError e = expectError("RECEIVER_LAT=1\nRECEIVER_LON=2\r\n");
    BOOST_TEST(e.line == 2);
    BOOST_TEST(contains(e.what(), "test.conf:2:"));
    BOOST_TEST(contains(e.what(), "malformed"));
    e = expectError("RECEIVER_LAT=1\nRECEIVER_LON=2\nRECORD_DIR=\"/x\"\r\n");
    BOOST_TEST(e.line == 3);
    // Like skycue-start.sh ([[:space:]] includes \r), a CR-only blank line or a
    // comment ending in CR is still a blank/comment line.
    Config c = parseConfigText("# comment\r\n\r\nRECEIVER_LAT=1\nRECEIVER_LON=2\n", "t");
    BOOST_TEST(c.receiver_lat == 1.0);
}
