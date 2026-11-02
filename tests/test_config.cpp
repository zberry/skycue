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
    for (const char* line : {"READSB_JSON_PORT=0", "WEB_PORT=65536", "WEB_PORT=-1", "WEB_PORT=+80",
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

BOOST_AUTO_TEST_CASE(range_checks_name_key_and_line)
{
    struct Case {
        const char* text;
        const char* key;
    };
    for (const Case& k : {Case{"KEYFRAME_INTERVAL_S=0.5", "KEYFRAME_INTERVAL_S"},
                          Case{"KEYFRAME_INTERVAL_S=0", "KEYFRAME_INTERVAL_S"},
                          Case{"MIN_INTERVAL_S=0.05", "MIN_INTERVAL_S"},
                          Case{"MIN_INTERVAL_S=-1", "MIN_INTERVAL_S"},
                          Case{"TRACK_TIMEOUT_S=0.5", "TRACK_TIMEOUT_S"},
                          Case{"MAX_RANGE_NM=0", "MAX_RANGE_NM"},
                          Case{"MAX_RANGE_NM=-10", "MAX_RANGE_NM"},
                          Case{"MIN_ELEVATION_DEG=-90.5", "MIN_ELEVATION_DEG"},
                          Case{"MIN_ELEVATION_DEG=91", "MIN_ELEVATION_DEG"},
                          Case{"MAX_INTERVAL_S=0.5", "MAX_INTERVAL_S"}}) {
        ConfigError e = expectError(std::string(kLatLon) + k.text + "\n");
        BOOST_TEST(e.line == 3, k.text);
        BOOST_CHECK_MESSAGE(contains(e.what(), k.key), k.text << ": " << e.what());
    }

    // MAX_INTERVAL_S below MIN_INTERVAL_S: named on the MAX_INTERVAL_S line.
    ConfigError e = expectError(std::string(kLatLon) + "MIN_INTERVAL_S=5\nMAX_INTERVAL_S=4\n");
    BOOST_TEST(e.line == 4);
    BOOST_TEST(contains(e.what(), "MAX_INTERVAL_S"));
    BOOST_TEST(contains(e.what(), "MIN_INTERVAL_S"));
    // Only MIN_INTERVAL_S given, above the default maximum: named on its line.
    e = expectError(std::string(kLatLon) + "MIN_INTERVAL_S=20\n");
    BOOST_TEST(e.line == 3);
    BOOST_TEST(contains(e.what(), "MAX_INTERVAL_S"));

    // Recording with no format.
    e = expectError(std::string(kLatLon) + "RECORD_DIR=/data/rec\nRECORD_FORMATS=\n");
    BOOST_TEST(e.line == 4);
    BOOST_TEST(contains(e.what(), "RECORD_FORMATS"));
    e = expectError(std::string(kLatLon) + "RECORD_DIR=/data/rec\nRECORD_FORMATS=\"  \"\n");
    BOOST_TEST(contains(e.what(), "RECORD_FORMATS"));
    // No formats is fine while recording is off.
    BOOST_TEST(parseConfigText(std::string(kLatLon) + "RECORD_FORMATS=\n", "t").record_formats.empty());
}

BOOST_AUTO_TEST_CASE(range_checks_accept_boundaries)
{
    Config c = parseConfigText(std::string(kLatLon) +
                                   "KEYFRAME_INTERVAL_S=1\n"
                                   "MIN_INTERVAL_S=0.1\n"
                                   "MAX_INTERVAL_S=0.1\n"
                                   "TRACK_TIMEOUT_S=1\n"
                                   "MAX_RANGE_NM=0.001\n"
                                   "MIN_ELEVATION_DEG=-90\n"
                                   "RECORD_DIR=/data/rec\n"
                                   "RECORD_FORMATS=csv\n",
                               "t");
    BOOST_TEST(c.keyframe_interval_s == 1.0);
    BOOST_TEST(c.min_interval_s == 0.1);
    BOOST_TEST(c.max_interval_s == 0.1);
    BOOST_TEST(c.track_timeout_s == 1.0);
    BOOST_TEST(c.max_range_nm == 0.001);
    BOOST_TEST(c.min_elevation_deg == -90.0);
    c = parseConfigText(std::string(kLatLon) + "MIN_ELEVATION_DEG=90\n", "t");
    BOOST_TEST(c.min_elevation_deg == 90.0);
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

BOOST_AUTO_TEST_CASE(cue_tcp_connect_parsed)
{
    std::string text = readExample();
    replaceOnce(text, "\nRECEIVER_LAT=\n", "\nRECEIVER_LAT=51.5\n");
    replaceOnce(text, "\nRECEIVER_LON=\n", "\nRECEIVER_LON=-0.12\n");
    BOOST_TEST(parseConfigText(text, "skycue.conf.example").cue_tcp_connect_targets.empty());

    Config c = parseConfigText(std::string(kLatLon) +
                                   "CUE_TCP_CONNECT=\"10.0.0.5:4000 cuehost:4001\"\n",
                               "t");
    BOOST_REQUIRE_EQUAL(c.cue_tcp_connect_targets.size(), 2u);
    BOOST_TEST(c.cue_tcp_connect_targets[0].first == "10.0.0.5");
    BOOST_TEST(c.cue_tcp_connect_targets[0].second == 4000);
    BOOST_TEST(c.cue_tcp_connect_targets[1].first == "cuehost");
    BOOST_TEST(c.cue_tcp_connect_targets[1].second == 4001);
    BOOST_TEST(c.cue_tcp_port == 30500);  // the listener stays on: both roles at once

    c = parseConfigText(std::string(kLatLon) + "CUE_TCP_CONNECT=\n", "t");
    BOOST_TEST(c.cue_tcp_connect_targets.empty());

    for (const char* v : {"\"x\"", "x", ":4000", "h:", "h:0", "h:65536", "h:12a"}) {
        ConfigError e = expectError(std::string(kLatLon) + "CUE_TCP_CONNECT=" + v + "\n");
        BOOST_TEST(e.line == 3, v);
        BOOST_TEST(contains(e.what(), "CUE_TCP_CONNECT"), v);
    }
}

BOOST_AUTO_TEST_CASE(cue_tcp_port_zero_allowed)
{
    Config c = parseConfigText(std::string(kLatLon) + "CUE_TCP_PORT=0\n", "t");
    BOOST_TEST(c.cue_tcp_port == 0);  // no listener

    for (const char* v : {"65536", "-1", "x", ""}) {
        ConfigError e = expectError(std::string(kLatLon) + "CUE_TCP_PORT=" + v + "\n");
        BOOST_TEST(e.line == 3, v);
        BOOST_TEST(contains(e.what(), "CUE_TCP_PORT"), v);
    }
    // WEB_PORT=0: no web UI.
    BOOST_TEST(parseConfigText(std::string(kLatLon) + "WEB_PORT=0\n", "t").web_port == 0);
    // Other ports still need 1-65535.
    ConfigError e = expectError(std::string(kLatLon) + "READSB_JSON_PORT=0\n");
    BOOST_TEST(contains(e.what(), "READSB_JSON_PORT"));
}

BOOST_AUTO_TEST_CASE(web_bind_record_max_gb_trail_seconds)
{
    Config c = parseConfigText(kLatLon, "t");
    BOOST_TEST(c.web_bind == "127.0.0.1");
    BOOST_TEST(c.record_max_gb == 0.0);
    BOOST_TEST(c.trail_seconds == 120);
    c = parseConfigText(std::string(kLatLon) + "WEB_BIND=0.0.0.0\nRECORD_MAX_GB=2.5\nTRAIL_SECONDS=3600\n", "t");
    BOOST_TEST(c.web_bind == "0.0.0.0");
    BOOST_TEST(c.record_max_gb == 2.5);
    BOOST_TEST(c.trail_seconds == 3600);
    BOOST_TEST(parseConfigText(std::string(kLatLon) + "TRAIL_SECONDS=0\n", "t").trail_seconds == 0);
    for (const char* line : {"TRAIL_SECONDS=3601", "TRAIL_SECONDS=-1", "TRAIL_SECONDS=1.5", "TRAIL_SECONDS=x"}) {
        ConfigError e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(e.line == 3, line);
        BOOST_TEST(contains(e.what(), "TRAIL_SECONDS"), line);
    }
    for (const char* line : {"RECORD_MAX_GB=-0.5", "RECORD_MAX_GB=x"}) {
        ConfigError e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(contains(e.what(), "RECORD_MAX_GB"), line);
    }
    ConfigError e = expectError(std::string(kLatLon) + "WEB_BIND=\n");
    BOOST_TEST(contains(e.what(), "WEB_BIND"));
}

BOOST_AUTO_TEST_CASE(fisb_enable_and_experimental)
{
    Config c = parseConfigText(kLatLon, "t");
    BOOST_TEST(c.fisb_enable);
    BOOST_TEST(!c.fisb_experimental);
    c = parseConfigText(std::string(kLatLon) + "FISB_ENABLE=no\nFISB_EXPERIMENTAL=yes\n", "t");
    BOOST_TEST(!c.fisb_enable);
    BOOST_TEST(c.fisb_experimental);
    for (const char* line : {"FISB_ENABLE=1", "FISB_ENABLE=", "FISB_EXPERIMENTAL=true"}) {
        ConfigError e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(e.line == 3, line);
        BOOST_TEST(contains(e.what(), "yes or no"), line);
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
